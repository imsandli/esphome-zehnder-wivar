#include "wivar_session.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace esphome::zehnder_wivar {

static const size_t MAX_FRAME = 4096;
static const uint32_t LOGIN_TIMEOUT_MS = 5000;
static const uint32_t VERSION_TIMEOUT_MS = 5000;
static const uint32_t ROOMS_TIMEOUT_MS = 15000;
static const uint32_t RUN_STATE_TIMEOUT_MS = 6000;
static const uint32_t WRITE_TIMEOUT_MS = 20000;
static const uint32_t PAIRING_TIMEOUT_MS = 35000;
static const uint32_t CLOSE_TIMEOUT_MS = 10000;
static const uint32_t PLAN_TIMEOUT_MS = 8000;
// Sessions a schedule change may write in (or fail to read the schedule) before it is given up.
static const uint8_t MAX_PLAN_ROUNDS = 3;
// The remote doesn't always push a reply by itself; after this much silence it is polled with a read.
static const uint32_t POKE_INTERVAL_MS = 1000;
// After a write the remote is busy relaying it to the heater and doesn't answer reads; read back in a new session.
// 5 s later the first connect failed after every schedule write (2026-10-14); the retry 30 s later worked. The
// session gap (default 20 s) applies on top, so this only matters with a shorter configured gap.
static const uint32_t READBACK_DELAY_MS = 10000;

// millis() wraps every ~49.7 days; compare by difference.
static bool reached(uint32_t now, uint32_t deadline) { return (int32_t) (now - deadline) >= 0; }

const char *bridge_status_text(BridgeStatus status) {
  switch (status) {
    case BridgeStatus::STARTING:
      return "Starting";
    case BridgeStatus::WAITING:
      return "Waiting";
    case BridgeStatus::CONNECTING:
      return "Connecting";
    case BridgeStatus::READING:
      return "Reading";
    case BridgeStatus::WRITING:
      return "Writing";
    case BridgeStatus::OK:
      return "OK";
    case BridgeStatus::WRITTEN:
      return "Written";
    case BridgeStatus::PAIRING_CONNECTING:
      return "Pairing: connecting";
    case BridgeStatus::PAIRING:
      return "Press MODE on remote!";
    case BridgeStatus::PAIRED:
      return "Paired";
    case BridgeStatus::PAIRING_FAILED:
      return "Pairing failed";
    case BridgeStatus::NOT_PAIRED:
      return "Not paired";
    case BridgeStatus::UNREACHABLE:
      return "Wivar not reachable";
    case BridgeStatus::CONNECT_FAILED:
      return "BLE connect failed";
    case BridgeStatus::DISCONNECTED:
      return "Disconnected";
    case BridgeStatus::TIMEOUT:
      return "Timeout";
    case BridgeStatus::ERROR:
      return "Error";
  }
  return "Unknown";
}

bool bridge_status_is_problem(BridgeStatus status) {
  switch (status) {
    case BridgeStatus::PAIRING_FAILED:
    case BridgeStatus::NOT_PAIRED:
    case BridgeStatus::UNREACHABLE:
    case BridgeStatus::CONNECT_FAILED:
    case BridgeStatus::DISCONNECTED:
    case BridgeStatus::TIMEOUT:
    case BridgeStatus::ERROR:
      return true;
    default:
      return false;
  }
}

Session::Session(Transport &transport, Clock &clock, SessionListener &listener, SessionConfig config)
    : transport_(transport), clock_(clock), listener_(listener), config_(std::move(config)) {}

void Session::log_(char level, const char *fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof buf, fmt, args);
  va_end(args);
  this->listener_.on_log(level, buf);
}

void Session::set_status_(BridgeStatus status, const std::string &detail) {
  std::string text = detail.empty() ? bridge_status_text(status) : detail;
  if (status == this->status_ && text == this->status_text_)
    return;
  this->status_ = status;
  this->status_text_ = text;
  bool in_session = status == BridgeStatus::CONNECTING || status == BridgeStatus::READING ||
                    status == BridgeStatus::WRITING || status == BridgeStatus::PAIRING_CONNECTING ||
                    status == BridgeStatus::PAIRING;
  if (!in_session) {
    this->settled_status_ = status;
    this->settled_text_ = text;
  }
  this->log_('I', "Status: %s", text.c_str());
  this->listener_.on_status(status, text);
}

uint32_t Session::reading_age_s() const {
  if (this->last_reading_ms_ == 0)
    return UINT32_MAX;
  return (this->clock_.millis() - this->last_reading_ms_) / 1000;
}

// ── Commands ─────────────────────────────────────────────────────────────────

void Session::poll() {
  // A running session reads the remote anyway; another one right after it would only fail more often (seen
  // 2026-10-14: the periodic poll fired during a "Refresh now" session and its connect 5 s later failed). Unless a
  // schedule read was asked for, which the running session may already be past.
  if (this->active_ && !(this->schedule_due_ && this->config_.want_schedule)) {
    this->log_('D', "Poll requested during a session: covered by it");
    return;
  }
  this->request_start_();
}

void Session::start_pairing() {
  this->log_('I', "Pairing requested");
  this->pair_requested_ = true;
  this->request_start_();
}

void Session::set_setpoint(uint8_t mode, int32_t tenths) {
  if (mode >= proto::NUM_SETPOINTS)
    return;
  tenths = this->clamp_setpoint_(mode, tenths);
  this->pending_setpoints_[mode] = tenths;
  this->log_('I', "New %s setpoint %.1f °C queued", proto::temp_mode_name(mode), tenths / 10.0f);
  this->request_start_();
}

void Session::set_active_target(int32_t tenths) {
  // Which mode is active is decided only after the write session has read the run state (resolve_active_target_()):
  // the schedule may have switched between Home and Away since the last poll.
  this->pending_active_target_ = tenths;
  this->log_('I', "New target %.1f °C for the active mode queued", tenths / 10.0f);
  this->request_start_();
}

void Session::resolve_active_target_(bool mode_known) {
  int32_t tenths = this->pending_active_target_;
  this->pending_active_target_ = -1;
  if (mode_known && this->temp_mode_ >= 0 && this->temp_mode_ < proto::NUM_SETPOINTS) {
    // Not set_setpoint(): this runs inside the write session, a start request would only add another session.
    this->pending_setpoints_[this->temp_mode_] = this->clamp_setpoint_(this->temp_mode_, tenths);
    return;
  }
  // Active mode unknown (or Antifreeze/Override): change every mode whose setpoint equals the current target.
  int32_t target = this->has_room_ && this->room_.has_target_temperature
                       ? (int32_t) std::lround(this->room_.target_temperature * 10.0f)
                       : -1;
  bool any = false;
  for (uint8_t mode = 0; mode < proto::NUM_SETPOINTS; mode++) {
    if (mode == proto::TEMP_MODE_ASLEEP)
      continue;  // never used by the Wivar
    if (this->has_room_ && this->room_.setpoints[mode] == target) {
      this->pending_setpoints_[mode] = this->clamp_setpoint_(mode, tenths);
      any = true;
    }
  }
  if (!any) {
    this->log_('W', "Active temperature mode unknown, changing the Home Awake setpoint");
    this->pending_setpoints_[0] = this->clamp_setpoint_(0, tenths);
  }
}

void Session::set_run_mode(RunModeChoice choice) {
  this->pending_run_mode_ = static_cast<int>(choice);
  static const char *const NAMES[] = {"Automatic", "Manual", "Antifreeze"};
  this->log_('I', "Run mode %s queued", NAMES[static_cast<int>(choice)]);
  this->request_start_();
}

int Session::active_week_plan() const {
  if (!this->has_schedule_)
    return -1;
  const proto::WeekPlan *week = active_week(this->schedule_, this->has_run_state_ ? this->run_state_ : proto::RunState{});
  return week != nullptr ? (int) week->id : -1;
}

std::string Session::start_target_(const ScheduleTarget &target) {
  if (!this->has_schedule_)
    return "schedule not read yet";
  if (this->has_target_)
    return "the previous schedule change is still being written";
  // Checked against the last read for a quick answer; the write session plans again from its own read.
  std::vector<PlanOp> ops;
  std::string error;
  if (!plan_target(this->schedule_, target, ops, error))
    return error;
  this->target_ = target;
  this->has_target_ = true;
  this->target_wrote_ = false;
  this->add_sent_ = false;
  this->round_clean_ = false;
  this->target_rounds_ = 0;
  this->log_('I', "Schedule change queued");
  this->request_start_();
  return "";
}

std::string Session::set_schedule(const std::string &text) {
  if (!this->has_schedule_)
    return "schedule not read yet";
  int week = this->active_week_plan();
  if (week < 0)
    return "the week plan in use is unknown";
  ScheduleTarget target;
  target.kind = ScheduleTarget::WEEK_TEXT;
  target.text = text;
  target.id = (uint32_t) week;
  return this->start_target_(target);
}

std::string Session::write_day_plan(const proto::DayPlan &plan) {
  if (!this->has_schedule_)
    return "schedule not read yet";
  if (this->schedule_.day_plan(plan.id) == nullptr)
    return "no day plan with id " + std::to_string(plan.id);
  std::string error = proto::validate_day_plan(plan);
  if (!error.empty())
    return error;
  ScheduleTarget target;
  target.kind = ScheduleTarget::DAY_PLAN;
  target.day = plan;
  return this->start_target_(target);
}

std::string Session::write_week_plan(const proto::WeekPlan &plan) {
  if (!this->has_schedule_)
    return "schedule not read yet";
  if (this->schedule_.week_plan(plan.id) == nullptr)
    return "no week plan with id " + std::to_string(plan.id);
  std::string error = proto::validate_week_plan(plan);
  if (!error.empty())
    return error;
  ScheduleTarget target;
  target.kind = ScheduleTarget::WEEK_PLAN;
  target.week = plan;
  return this->start_target_(target);
}

std::string Session::add_day_plan(const proto::DayPlan &plan) {
  std::string error = proto::validate_day_plan(plan);
  if (!error.empty())
    return error;
  ScheduleTarget target;
  target.kind = ScheduleTarget::ADD_DAY;
  target.day = plan;
  return this->start_target_(target);
}

std::string Session::remove_day_plan(uint32_t id) {
  if (!this->has_schedule_)
    return "schedule not read yet";
  if (this->schedule_.day_plan(id) == nullptr)
    return "no day plan with id " + std::to_string(id);
  ScheduleTarget target;
  target.kind = ScheduleTarget::REMOVE_DAY;
  target.id = id;
  return this->start_target_(target);
}

std::string Session::add_week_plan(const proto::WeekPlan &plan) {
  std::string error = proto::validate_week_plan(plan);
  if (!error.empty())
    return error;
  ScheduleTarget target;
  target.kind = ScheduleTarget::ADD_WEEK;
  target.week = plan;
  return this->start_target_(target);
}

std::string Session::remove_week_plan(uint32_t id) {
  if (!this->has_schedule_)
    return "schedule not read yet";
  if (this->schedule_.week_plan(id) == nullptr)
    return "no week plan with id " + std::to_string(id);
  if ((int) id == this->active_week_plan())
    return "week plan " + std::to_string(id) + " is in use";
  ScheduleTarget target;
  target.kind = ScheduleTarget::REMOVE_WEEK;
  target.id = id;
  return this->start_target_(target);
}

int32_t Session::clamp_setpoint_(uint8_t mode, int32_t tenths) const {
  int32_t lo = std::max(this->config_.min_setpoint_tenths, proto::SETPOINT_MIN[mode]);
  int32_t hi = std::min(this->config_.max_setpoint_tenths, proto::SETPOINT_MAX[mode]);
  return std::clamp(tenths, lo, std::max(lo, hi));
}

void Session::start_timer(uint32_t minutes) {
  this->pending_timer_ = minutes;
  if (minutes > 0)
    this->log_('I', "Heating timer %u min queued", (unsigned) minutes);
  else
    this->log_('I', "Heating timer stop queued");
  this->request_start_();
}

// ── Scheduling ───────────────────────────────────────────────────────────────

void Session::request_start_() { this->start_requested_ = true; }

void Session::schedule_start_(uint32_t delay_ms) {
  uint32_t at = this->clock_.millis() + delay_ms;
  if (!this->start_scheduled_ || reached(this->start_at_, at))  // keep the earlier one
    this->start_at_ = at;
  this->start_scheduled_ = true;
}

void Session::loop() {
  uint32_t now = this->clock_.millis();
  if (!this->active_) {
    bool due = this->start_requested_ || (this->start_scheduled_ && reached(now, this->start_at_));
    bool gap_over = !this->ended_once_ || now - this->ended_at_ >= this->config_.session_gap_ms;
    if (due && gap_over && now >= this->config_.boot_delay_ms && this->transport_.idle())
      this->start_();
    return;
  }
  bool waiting_for_reply = this->sent_ && this->step_ != Step::NONE && this->step_ != Step::CONNECTING &&
                           this->step_ != Step::CLOSING;
  if (waiting_for_reply) {
    if (this->read_pending_ && now - this->last_activity_ > 3 * POKE_INTERVAL_MS)
      this->read_pending_ = false;  // the read response got lost
    if (!this->read_pending_ && now - this->last_activity_ >= POKE_INTERVAL_MS) {
      this->read_pending_ = true;
      this->last_activity_ = now;
      this->transport_.poke();
    }
  }
  if (!reached(now, this->deadline_))
    return;
  switch (this->step_) {
    case Step::CLOSING:
      this->end_();  // no disconnect event arrived; release the session anyway
      break;
    case Step::CONNECTING:
      this->finish_(BridgeStatus::UNREACHABLE);
      break;
    default:
      this->step_timed_out_();
      break;
  }
}

void Session::start_() {
  this->start_requested_ = false;
  this->start_scheduled_ = false;
  this->active_ = true;
  this->started_ = false;
  this->plan_.clear();
  this->step_ = Step::CONNECTING;
  this->deadline_ = this->clock_.millis() + this->config_.connect_timeout_ms;
  this->set_status_(this->pair_requested_ ? BridgeStatus::PAIRING_CONNECTING : BridgeStatus::CONNECTING);
  this->transport_.connect();
}

// ── Transport events ─────────────────────────────────────────────────────────

void Session::on_connected() {
  if (this->active_ && !this->started_ && this->step_ == Step::CONNECTING)
    this->begin_();
}

void Session::on_connect_failed() {
  if (this->active_ && this->step_ == Step::CONNECTING)
    this->finish_(BridgeStatus::CONNECT_FAILED);
}

void Session::on_disconnected() {
  if (!this->active_)
    return;
  if (this->step_ != Step::CLOSING) {
    // The link dropped (or never came up) without finish_(): treat it as a failed poll.
    this->note_result_(this->step_ == Step::CONNECTING ? BridgeStatus::UNREACHABLE : BridgeStatus::DISCONNECTED, "");
  }
  this->end_();
}

void Session::on_sent() {
  this->sent_ = true;
  this->last_activity_ = this->clock_.millis();
}

void Session::on_link_error(const std::string &detail) {
  if (this->active_ && this->step_ != Step::CLOSING)
    this->finish_(BridgeStatus::ERROR, detail);
}

void Session::on_read_done() { this->read_pending_ = false; }

void Session::on_data(const uint8_t *data, size_t len) {
  if (!this->active_)
    return;
  this->last_activity_ = this->clock_.millis();
  this->rx_.insert(this->rx_.end(), data, data + len);
  while (this->active_) {
    size_t size = proto::frame_size(this->rx_);
    if (size > MAX_FRAME) {
      this->log_('W', "Frame too large, dropping");
      this->rx_.clear();
      return;
    }
    if (size == 0 || this->rx_.size() < size) {
      // The remote delivers the next chunk as a notification after each read of the characteristic.
      this->read_pending_ = true;
      this->transport_.poke();
      return;
    }
    std::vector<uint8_t> frame(this->rx_.begin(), this->rx_.begin() + size);
    this->rx_.erase(this->rx_.begin(), this->rx_.begin() + size);
    this->listener_.on_log('V', "RX " + proto::to_hex(frame));
    proto::Frame parsed;
    if (proto::parse_frame(frame.data(), frame.size(), parsed))
      this->handle_frame_(parsed);
    else
      this->log_('W', "Malformed frame dropped");
    if (this->rx_.empty())
      return;
  }
}

// ── Steps ────────────────────────────────────────────────────────────────────

bool Session::writes_pending_() const {
  return this->pending_timer_ >= 0 || this->pending_run_mode_ >= 0 || this->setpoints_pending_() ||
         this->pending_active_target_ >= 0;
}

bool Session::setpoints_pending_() const {
  for (int32_t v : this->pending_setpoints_)
    if (v >= 0)
      return true;
  return false;
}

void Session::plan_writes_() {
  this->plan_.push_back(Step::SET_SETPOINTS);
  // Run mode before the timer: writing the run state ends a running timer on the remote.
  this->plan_.push_back(Step::SET_RUN_STATE);
  this->plan_.push_back(Step::SET_TIMER);
  this->plan_.push_back(Step::SET_PLANS);
}

void Session::begin_() {
  this->started_ = true;
  this->connected_at_ = this->clock_.millis();
  this->stats_.connections++;
  this->wrote_ = false;
  this->need_readback_ = false;
  this->retried_writes_ = false;
  this->schedule_fresh_ = false;
  this->target_in_session_ = this->has_target_;
  this->target_planned_ = false;
  this->run_state_fresh_ = false;
  this->plan_.clear();
  if (this->pair_requested_) {
    this->pair_requested_ = false;
    this->plan_.push_back(Step::PAIRING);
  }
  this->plan_.push_back(Step::LOGIN);
  this->plan_.push_back(Step::VERSION);
  if (this->target_in_session_) {
    // A schedule change is planned from a read in the same session, before any write: the remote may have
    // changed since the last read (app, an earlier interrupted round).
    this->schedule_due_ = true;
    this->plan_.push_back(Step::GET_SCHEDULE_IDS);
  }
  if (this->setpoints_pending_() || this->pending_active_target_ >= 0) {
    // A setpoint write carries every mode: read the current setpoints first, so a change made on the remote or in
    // the app since the last poll isn't overwritten with a cached value. handle_rooms_() then plans the writes.
    // The run state comes first here as in every plan: a thermostat target goes to the mode active now.
    this->plan_.push_back(Step::GET_RUN_STATE);
    this->plan_.push_back(Step::GET_ROOMS);
    this->next_step_();
    return;
  }
  this->plan_writes_();
  this->plan_.push_back(Step::GET_RUN_STATE);
  this->plan_.push_back(Step::GET_ROOMS);
  this->plan_.push_back(Step::GET_SCHEDULE_IDS);
  this->plan_.push_back(Step::GET_TIME);
  this->next_step_();
}

void Session::send_(const std::vector<uint8_t> &frame, uint32_t expect, uint32_t timeout_ms) {
  // rx_ is kept: the remote may still be delivering a frame that follows the one that ended the last step (e.g. a
  // confirm behind an indication, sharing a chunk with it). Dropping it would misframe every chunk after it.
  this->sent_ = false;
  this->read_pending_ = false;
  this->expect_ = expect;
  this->deadline_ = this->clock_.millis() + timeout_ms;
  this->last_activity_ = this->clock_.millis();
  // Decided by the step, not by parsing the frame: a change to these frames can't make them show up in the log.
  this->transport_.send(frame, this->step_ == Step::LOGIN || this->step_ == Step::PAIRING);
}

void Session::next_step_() {
  while (!this->plan_.empty()) {
    this->step_ = this->plan_.front();
    this->plan_.pop_front();
    switch (this->step_) {
      case Step::PAIRING: {
        uint8_t nonce[16];
        for (int i = 0; i < 16; i += 4) {
          uint32_t r = this->clock_.random_u32();
          std::memcpy(nonce + i, &r, 4);
        }
        this->set_status_(BridgeStatus::PAIRING);
        this->send_(proto::build_pairing_request(this->reference_++, this->config_.app_uuid, nonce,
                                                 this->config_.nickname),
                    proto::OP_APP_PAIRING_CONFIRM, PAIRING_TIMEOUT_MS);
        return;
      }
      case Step::LOGIN:
        this->send_(proto::build_set_address(this->reference_++, this->config_.app_uuid), proto::OP_SET_ADDRESS_CONFIRM,
                    LOGIN_TIMEOUT_MS);
        return;
      case Step::VERSION:
        if (this->have_version_ || !this->config_.want_version)
          continue;
        this->send_(proto::build_request(proto::OP_VERSION_REQUEST, this->reference_++), proto::OP_VERSION_CONFIRM,
                    VERSION_TIMEOUT_MS);
        return;
      case Step::SET_SETPOINTS: {
        if (!this->setpoints_pending_())
          continue;
        // The write carries all setpoints, so the current ones must be known.
        int32_t merged[proto::NUM_SETPOINTS];
        bool complete = this->has_room_;
        for (int mode = 0; mode < proto::NUM_SETPOINTS; mode++) {
          merged[mode] = this->pending_setpoints_[mode] >= 0 ? this->pending_setpoints_[mode] : this->room_.setpoints[mode];
          complete &= merged[mode] >= 0;
        }
        if (!complete) {
          this->log_('W', "Current setpoints not read yet, write postponed");
          continue;
        }
        this->log_('I', "Writing setpoints: Home %.1f, Asleep %.1f, Away %.1f °C", merged[0] / 10.0f,
                   merged[1] / 10.0f, merged[2] / 10.0f);
        this->set_status_(BridgeStatus::WRITING);
        this->wrote_ = true;
        std::copy(std::begin(merged), std::end(merged), std::begin(this->written_setpoints_));
        std::copy(std::begin(this->pending_setpoints_), std::end(this->pending_setpoints_),
                  std::begin(this->sent_setpoints_));
        this->send_(proto::build_set_setpoints(this->reference_++, this->room_.id, merged),
                    proto::OP_SET_ROOM_VALUE_CONFIRM, WRITE_TIMEOUT_MS);
        return;
      }
      case Step::SET_TIMER:
        if (this->pending_timer_ < 0)
          continue;
        if (!this->has_room_) {
          this->log_('W', "Device not read yet, timer postponed");
          continue;
        }
        this->log_('I', "Setting heating timer: %d min", (int) this->pending_timer_);
        this->set_status_(BridgeStatus::WRITING);
        this->wrote_ = true;
        this->sent_timer_ = this->pending_timer_;
        this->send_(proto::build_preheat_timer(this->reference_++, this->room_.device_id, this->sent_timer_),
                    proto::OP_SET_DEVICE_FUNCTION_MODE_CONFIRM, WRITE_TIMEOUT_MS);
        return;
      case Step::SET_RUN_STATE: {
        if (this->pending_run_mode_ < 0)
          continue;
        auto choice = static_cast<RunModeChoice>(this->pending_run_mode_);
        uint32_t mode = choice == RunModeChoice::AUTOMATIC    ? proto::RUN_MODE_AUTOMATIC
                        : choice == RunModeChoice::ANTIFREEZE ? proto::RUN_MODE_ANTIFREEZE
                                                              : proto::RUN_MODE_MANUAL;
        // Like the app: pass the location matching the current temperature mode.
        bool away = this->temp_mode_ == 2;
        this->log_('I', "Setting run mode %s, %s", proto::run_mode_name(mode), away ? "Away" : "Home");
        this->set_status_(BridgeStatus::WRITING);
        this->wrote_ = true;
        this->written_run_mode_ = mode;
        this->sent_run_mode_ = this->pending_run_mode_;
        this->send_(proto::build_set_run_state(this->reference_++, mode, away ? proto::LOCATION_AWAY : proto::LOCATION_HOME),
                    proto::OP_SET_RUN_STATE_CONFIRM, WRITE_TIMEOUT_MS);
        return;
      }
      case Step::SET_PLANS:
        if (this->next_plan_write_())
          return;
        continue;
      case Step::GET_ROOMS:
        if (this->wrote_) {
          // The remote answers setpoint writes with the complete new room record; only writes without one
          // (timer, unconfirmed) need a read-back.
          this->finish_(BridgeStatus::WRITTEN);
          if (this->need_readback_)
            this->schedule_start_(READBACK_DELAY_MS);
          return;
        }
        if (this->status_ != BridgeStatus::WRITING)
          this->set_status_(BridgeStatus::READING);
        this->send_(proto::build_request(proto::OP_GET_ROOMS_REQUEST, this->reference_++), proto::OP_GET_ROOMS_CONFIRM,
                    ROOMS_TIMEOUT_MS);
        return;
      case Step::GET_RUN_STATE:
        if (this->wrote_)
          continue;  // the session ends at GET_ROOMS, the read-back reads it
        this->send_(proto::build_request(proto::OP_GET_RUN_STATE_REQUEST, this->reference_++),
                    proto::OP_GET_RUN_STATE_CONFIRM, RUN_STATE_TIMEOUT_MS);
        return;
      case Step::GET_SCHEDULE_IDS:
        if (!this->config_.want_schedule && !this->target_in_session_)
          continue;
        if (!this->schedule_due_ &&
            !(this->has_schedule_ &&
              this->clock_.millis() - this->schedule_read_at_ >= this->config_.schedule_interval_ms))
          continue;
        this->schedule_new_ = Schedule{};
        this->plan_requests_.clear();
        this->send_(proto::build_request(proto::OP_GET_SCHEDULE_IDS_REQUEST, this->reference_++),
                    proto::OP_GET_SCHEDULE_IDS_CONFIRM, PLAN_TIMEOUT_MS);
        return;
      case Step::GET_TIME:
        if (!this->config_.check_time || this->clock_.unix_now() == 0)
          continue;
        if (!this->time_due_ && this->clock_.millis() - this->time_done_at_ < this->config_.time_sync_interval_ms)
          continue;
        this->have_remote_time_ = false;
        if (this->get_time_unsupported_) {
          this->plan_.push_front(Step::SET_TIME);
          continue;
        }
        this->send_(proto::build_request(proto::OP_GET_TIME_REQUEST, this->reference_++), proto::OP_GET_TIME_CONFIRM,
                    PLAN_TIMEOUT_MS);
        return;
      case Step::SET_TIME: {
        uint64_t now = this->clock_.unix_now();
        bool summer = false;
        std::vector<proto::DstSwitch> switches;
        if (!this->config_.sync_time || now == 0 || !this->clock_.dst_info(now, summer, switches)) {
          this->time_done_();
          continue;
        }
        // Only when off: the time, the current daylight saving, or the next switch the remote knows.
        const char *reason = this->have_remote_time_ ? nullptr : "no clock read, daily sync";
        uint64_t off = this->remote_offset_ < 0 ? -this->remote_offset_ : this->remote_offset_;
        if (reason != nullptr) {
        } else if (off > this->config_.time_tolerance_s) {
          reason = "time off";
        } else if (this->remote_time_.daylight_saving != (summer ? 3u : 2u)) {
          reason = "daylight saving differs";
        } else if (!switches.empty()) {
          bool known = false;
          for (auto &sw : this->remote_time_.switches)
            known |= sw.local_time == switches[0].local_time && sw.summer_after == switches[0].summer_after;
          if (!known)
            reason = "next daylight-saving switch missing";
        }
        if (reason == nullptr) {
          this->log_('D', "Remote clock is correct");
          this->time_done_();
          continue;
        }
        this->log_('I', "Setting the remote's clock (%s; %s time, %u daylight-saving switches)", reason,
                   summer ? "summer" : "winter", (unsigned) switches.size());
        this->send_(proto::build_set_time(this->reference_++, now, summer, switches), proto::OP_SET_TIME_CONFIRM,
                    PLAN_TIMEOUT_MS);
        return;
      }
      case Step::GET_PLAN:
        if (this->next_plan_request_())
          return;
        // All plans read.
        this->schedule_ = std::move(this->schedule_new_);
        this->has_schedule_ = true;
        this->schedule_due_ = false;
        this->schedule_read_at_ = this->clock_.millis();
        this->log_('I', "Schedule read: %u day plans, %u week plans", (unsigned) this->schedule_.day_plans.size(),
                   (unsigned) this->schedule_.week_plans.size());
        for (auto &p : this->schedule_.day_plans)
          this->log_('D', "  day plan %u '%s': %s", (unsigned) p.id, p.name.c_str(), proto::describe_day_plan(p).c_str());
        for (auto &w : this->schedule_.week_plans)
          this->log_('D', "  week plan %u '%s': %u %u %u %u %u %u %u", (unsigned) w.id, w.name.c_str(),
                     (unsigned) w.day_ids[0], (unsigned) w.day_ids[1], (unsigned) w.day_ids[2], (unsigned) w.day_ids[3],
                     (unsigned) w.day_ids[4], (unsigned) w.day_ids[5], (unsigned) w.day_ids[6]);
        this->schedule_fresh_ = true;
        this->listener_.on_schedule(this->schedule_);
        continue;
      default:
        continue;
    }
  }
  this->finish_(BridgeStatus::OK);
}

void Session::step_timed_out_() {
  this->rx_.clear();  // a reply that stopped midway must not be read as the start of the next one
  switch (this->step_) {
    case Step::GET_ROOMS:
      this->finish_(BridgeStatus::TIMEOUT);
      return;
    case Step::PAIRING:
      this->finish_(BridgeStatus::PAIRING_FAILED, "Pairing timed out");
      return;
    case Step::SET_TIMER:
      // Not counted as started; the read-back shows whether the remote runs it.
      this->log_('W', "No confirmation for the timer, checking with a read");
      this->write_done_(false);
      return;
    case Step::SET_SETPOINTS:
      // Seen in practice: the write is applied without a confirm. The read-back shows the real values.
      this->log_('W', "No confirmation for the setpoint write, checking with a read");
      this->write_done_(false);
      return;
    case Step::SET_RUN_STATE:
      this->log_('W', "No confirmation for the run mode, checking with a read");
      this->write_done_(false);
      return;
    case Step::SET_PLANS:
      this->plan_write_done_(nullptr, false);
      return;
    case Step::GET_RUN_STATE:
      this->log_('W', "No run state from the remote");
      this->run_state_errors_.clear();
      break;
    case Step::GET_SCHEDULE_IDS:
    case Step::GET_PLAN:
      // Keep the previous schedule; try again in the next session.
      this->log_('W', "Schedule read incomplete, keeping the previous one");
      this->plan_requests_.clear();
      while (!this->plan_.empty() && this->plan_.front() == Step::GET_PLAN)
        this->plan_.pop_front();
      break;
    case Step::GET_TIME:
      // The Wivar doesn't implement GetTime: don't ask again, set the clock daily without a check.
      this->log_('I', "The remote doesn't report its clock; it will be set once a day");
      this->get_time_unsupported_ = true;
      this->plan_.push_front(Step::SET_TIME);
      break;
    case Step::SET_TIME:
      this->log_('W', "No confirmation for the clock");
      this->time_done_();
      break;
    default:
      this->log_('D', "No answer to optional request (expected type %u)", (unsigned) this->expect_);
      break;
  }
  this->next_step_();
}

void Session::write_done_(bool accepted, bool with_room) {
  if (!with_room)
    this->need_readback_ = true;
  // Only what this write carried is done; a command queued since then stays pending for the next session (its
  // request already asked for one).
  if (this->step_ == Step::SET_TIMER) {
    if (accepted && this->sent_timer_ > 0) {
      this->timer_until_ms_ = this->clock_.millis() + this->sent_timer_ * 60000u;
      if (this->timer_until_ms_ == 0)
        this->timer_until_ms_ = 1;
      this->timer_end_ = 0;  // a replaced timer's end time; the read-back reports the new one
      this->timer_checked_ = false;
    } else if (accepted) {
      // Stopped: forget both countdowns until the remote reports otherwise.
      this->timer_until_ms_ = 0;
      this->timer_end_ = 0;
    }
    if (this->pending_timer_ == this->sent_timer_)
      this->pending_timer_ = -1;
  } else if (this->step_ == Step::SET_RUN_STATE) {
    if (this->pending_run_mode_ == this->sent_run_mode_)
      this->pending_run_mode_ = -1;
  } else {
    for (int mode = 0; mode < proto::NUM_SETPOINTS; mode++)
      if (this->pending_setpoints_[mode] == this->sent_setpoints_[mode])
        this->pending_setpoints_[mode] = -1;
  }
  this->next_step_();
}

uint32_t Session::timer_remaining_s() const {
  // The remote's end time wins whenever it lies in the future (also covers timers started on the remote).
  uint64_t now = this->clock_.unix_now();
  if (now != 0 && this->timer_end_ > now)
    return this->timer_end_ - now;
  // Otherwise: a timer started from here that the remote hasn't reported yet.
  if (this->timer_until_ms_ != 0) {
    int32_t left = (int32_t) (this->timer_until_ms_ - this->clock_.millis());
    if (left > 0)
      return (left + 999) / 1000;
  }
  return 0;
}

// ── Frames ───────────────────────────────────────────────────────────────────

void Session::handle_frame_(const proto::Frame &frame) {
  this->log_('D', "Frame type=%u result=%u ref=%u body=%u bytes", (unsigned) frame.op_type, (unsigned) frame.result,
             (unsigned) frame.reference, (unsigned) frame.body_len);
  if (this->step_ == Step::SET_SETPOINTS && frame.op_type == proto::OP_ROOMS_UPDATE_INDICATION && this->sent_) {
    // The remote answers a setpoint write with a rooms update instead of the confirm. Only accept one that already
    // carries the written values, not an unrelated update.
    std::vector<proto::Room> rooms;
    if (proto::parse_rooms(frame.body, frame.body_len, rooms) && !rooms.empty()) {
      const auto &room = rooms[std::min<size_t>(this->config_.room_index, rooms.size() - 1)];
      if (std::equal(std::begin(room.setpoints), std::end(room.setpoints), std::begin(this->written_setpoints_))) {
        this->log_('I', "Setpoints written");
        this->apply_room_(room);  // like the app: the update is the read-back
        this->write_done_(true, true);
        return;
      }
    }
    this->log_('D', "Rooms update without the written setpoints, still waiting");
    return;
  }
  if (this->step_ == Step::SET_PLANS && frame.op_type == proto::OP_SCHEDULE_UPDATE_INDICATION) {
    this->log_('D', "Schedule update while writing a plan");  // the confirm follows; the read-back verifies
    return;
  }
  if (this->step_ == Step::SET_RUN_STATE && frame.op_type == proto::OP_RUN_STATE_UPDATE_INDICATION && this->sent_) {
    // Possibly the remote's answer instead of the confirm (as for setpoints): accept it if it shows the new mode.
    proto::RunState rs;
    if (proto::parse_run_state(frame.body, frame.body_len, rs) && rs.run_mode == this->written_run_mode_) {
      this->log_('I', "Run mode written");
      this->write_done_(true);
      return;
    }
    this->log_('D', "Run state update without the written mode, still waiting");
    return;
  }
  if (frame.op_type != this->expect_) {
    this->log_('D', "Ignoring frame type %u (waiting for %u)", (unsigned) frame.op_type, (unsigned) this->expect_);
    return;
  }
  switch (this->step_) {
    case Step::LOGIN:
      if (frame.result != proto::RESULT_OK)
        this->log_('W', "Login (SetAddress) result %u", (unsigned) frame.result);
      this->next_step_();
      break;
    case Step::VERSION: {
      uint32_t version;
      if (frame.result == proto::RESULT_OK && proto::parse_version(frame.body, frame.body_len, version)) {
        this->have_version_ = true;
        bool tested = version == TESTED_REMOTE_FIRMWARE;
        if (!tested)
          this->log_('W', "Remote firmware 0x%06X differs from the tested 0x%06X; check that readings and writes work",
                     (unsigned) version, (unsigned) TESTED_REMOTE_FIRMWARE);
        this->listener_.on_version(version, tested);
      }
      this->next_step_();
      break;
    }
    case Step::PAIRING:
      if (frame.result != proto::RESULT_OK) {
        this->finish_(BridgeStatus::PAIRING_FAILED, "Pairing rejected (" + std::to_string(frame.result) + ")");
        return;
      }
      this->log_('I', "Pairing confirmed by Wivar");
      this->set_status_(BridgeStatus::PAIRED);
      this->next_step_();
      break;
    case Step::SET_SETPOINTS:
      if (frame.result == proto::RESULT_OK)
        this->log_('I', "Setpoints written");
      else
        this->log_('W', "Setpoint write rejected (result %u)", (unsigned) frame.result);
      this->write_done_(frame.result == proto::RESULT_OK);
      break;
    case Step::SET_TIMER:
      if (frame.result == proto::RESULT_OK)
        this->log_('I', "Timer written");
      else
        this->log_('W', "Timer rejected (result %u)", (unsigned) frame.result);
      this->write_done_(frame.result == proto::RESULT_OK);
      break;
    case Step::GET_ROOMS:
      this->handle_rooms_(frame);
      break;
    case Step::GET_RUN_STATE:
      this->handle_run_state_(frame);
      break;
    case Step::SET_RUN_STATE:
      if (frame.result == proto::RESULT_OK)
        this->log_('I', "Run mode written");
      else
        this->log_('W', "Run mode rejected (result %u)", (unsigned) frame.result);
      this->write_done_(frame.result == proto::RESULT_OK);
      break;
    case Step::GET_SCHEDULE_IDS:
    case Step::GET_PLAN:
      this->handle_plan_(frame);
      break;
    case Step::SET_PLANS:
      this->plan_write_done_(&frame, frame.result == proto::RESULT_OK);
      break;
    case Step::GET_TIME: {
      proto::RemoteTime t;
      if (frame.result == proto::RESULT_OK && proto::parse_time(frame.body, frame.body_len, t)) {
        int64_t offset = (int64_t) t.utc - (int64_t) this->clock_.unix_now();
        this->log_('I', "Remote clock: offset %lld s, daylight saving %u, %u switches stored", (long long) offset,
                   (unsigned) t.daylight_saving, (unsigned) t.switches.size());
        for (auto &sw : t.switches)
          this->log_('D', "  switch at local %llu to %s", (unsigned long long) sw.local_time,
                     sw.summer_after ? "summer" : "winter");
        bool summer = false;
        std::vector<proto::DstSwitch> ours;
        if (this->clock_.dst_info(this->clock_.unix_now(), summer, ours)) {
          this->log_('D', "Local daylight saving: %s", summer ? "summer" : "winter");
          for (auto &sw : ours)
            this->log_('D', "  switch at local %llu to %s", (unsigned long long) sw.local_time,
                       sw.summer_after ? "summer" : "winter");
        }
        this->listener_.on_clock_offset(offset);
        this->remote_time_ = std::move(t);
        this->remote_offset_ = offset;
        this->have_remote_time_ = true;
        this->plan_.push_front(Step::SET_TIME);
      } else {
        this->log_('W', "Clock read failed (result %u)", (unsigned) frame.result);
      }
      this->next_step_();
      break;
    }
    case Step::SET_TIME:
      if (frame.result == proto::RESULT_OK)
        this->log_('I', "Remote clock set");
      else
        this->log_('W', "Setting the clock was rejected (result %u)", (unsigned) frame.result);
      this->time_done_();
      this->next_step_();
      break;
    default:
      break;
  }
}

void Session::handle_plan_(const proto::Frame &frame) {
  bool ok = frame.result == proto::RESULT_OK;
  if (ok && this->step_ == Step::GET_SCHEDULE_IDS) {
    std::vector<uint32_t> day_ids, week_ids;
    ok = proto::parse_schedule_ids(frame.body, frame.body_len, day_ids, week_ids);
    for (uint32_t id : day_ids)
      this->plan_requests_.emplace_back(proto::OP_GET_DAY_PLAN_REQUEST, id);
    for (uint32_t id : week_ids)
      this->plan_requests_.emplace_back(proto::OP_GET_WEEK_PLAN_REQUEST, id);
  } else if (ok && frame.op_type == proto::OP_GET_DAY_PLAN_CONFIRM) {
    proto::DayPlan plan;
    ok = proto::parse_day_plan(frame.body, frame.body_len, plan);
    if (ok)
      this->schedule_new_.day_plans.push_back(std::move(plan));
  } else if (ok) {
    proto::WeekPlan plan;
    ok = proto::parse_week_plan(frame.body, frame.body_len, plan);
    if (ok)
      this->schedule_new_.week_plans.push_back(std::move(plan));
  }
  if (!ok) {
    this->log_('W', "Schedule read failed (type %u, result %u), keeping the previous one", (unsigned) frame.op_type,
               (unsigned) frame.result);
    this->plan_requests_.clear();
    this->next_step_();
    return;
  }
  this->plan_.push_front(Step::GET_PLAN);
  this->next_step_();
}

bool Session::next_plan_write_() {
  if (this->plan_ops_.empty() && !this->plan_round_())
    return false;
  PlanOp &op = this->plan_ops_.front();
  this->set_status_(BridgeStatus::WRITING);
  this->wrote_ = true;
  uint32_t ref = this->reference_++;
  switch (op.kind) {
    case PlanOp::SET_DAY:
      this->log_('I', "Writing day plan %u '%s': %s", (unsigned) op.day.id, op.day.name.c_str(),
                 proto::describe_day_plan(op.day).c_str());
      this->send_(proto::build_set_day_plan(ref, op.day), proto::OP_SET_DAY_PLAN_CONFIRM, WRITE_TIMEOUT_MS);
      break;
    case PlanOp::ADD_DAY:
      this->log_('I', "Adding day plan '%s': %s", op.day.name.c_str(), proto::describe_day_plan(op.day).c_str());
      this->add_sent_ |= this->target_.kind == ScheduleTarget::ADD_DAY;
      this->send_(proto::build_add_day_plan(ref, op.day), proto::OP_ADD_DAY_PLAN_CONFIRM, WRITE_TIMEOUT_MS);
      break;
    case PlanOp::REMOVE_DAY:
      this->log_('I', "Removing day plan %u", (unsigned) op.id);
      this->send_(proto::build_remove_plan(proto::OP_REMOVE_DAY_PLAN_REQUEST, ref, op.id),
                  proto::OP_REMOVE_DAY_PLAN_CONFIRM, WRITE_TIMEOUT_MS);
      break;
    case PlanOp::SET_WEEK:
      for (int d = 0; d < 7; d++)
        if (op.day_slot[d] >= 0)
          op.week.day_ids[d] = this->new_ids_[op.day_slot[d]];
      this->log_('I', "Writing week plan %u: %u %u %u %u %u %u %u", (unsigned) op.week.id,
                 (unsigned) op.week.day_ids[0], (unsigned) op.week.day_ids[1], (unsigned) op.week.day_ids[2],
                 (unsigned) op.week.day_ids[3], (unsigned) op.week.day_ids[4], (unsigned) op.week.day_ids[5],
                 (unsigned) op.week.day_ids[6]);
      this->send_(proto::build_set_week_plan(ref, op.week), proto::OP_SET_WEEK_PLAN_CONFIRM, WRITE_TIMEOUT_MS);
      break;
    case PlanOp::ADD_WEEK:
      this->log_('I', "Adding week plan '%s'", op.week.name.c_str());
      this->add_sent_ |= this->target_.kind == ScheduleTarget::ADD_WEEK;
      this->send_(proto::build_add_week_plan(ref, op.week), proto::OP_ADD_WEEK_PLAN_CONFIRM, WRITE_TIMEOUT_MS);
      break;
    case PlanOp::REMOVE_WEEK:
      this->log_('I', "Removing week plan %u", (unsigned) op.id);
      this->send_(proto::build_remove_plan(proto::OP_REMOVE_WEEK_PLAN_REQUEST, ref, op.id),
                  proto::OP_REMOVE_WEEK_PLAN_CONFIRM, WRITE_TIMEOUT_MS);
      break;
  }
  return true;
}

// Plans this session's round of the schedule change from the schedule read at its start. False if nothing is to be
// written (no change, already planned, done, failed, or the read is missing).
bool Session::plan_round_() {
  if (!this->has_target_ || !this->target_in_session_ || this->target_planned_)
    return false;
  this->target_planned_ = true;
  if (!this->schedule_fresh_) {
    if (++this->target_rounds_ >= MAX_PLAN_ROUNDS) {
      this->target_done_(ScheduleWriteResult::FAILED, "the schedule could not be read");
      return false;
    }
    this->log_('W', "Schedule not read, the change waits for the next session");
    this->schedule_start_(this->config_.retry_delay_ms);
    return false;
  }
  if (this->add_sent_) {
    this->target_done_(ScheduleWriteResult::FAILED, "interrupted; the plan may have been added, check the schedule");
    return false;
  }
  auto &released = this->target_.released;
  released.erase(std::remove_if(released.begin(), released.end(),
                                [this](uint32_t id) { return this->schedule_.day_plan(id) == nullptr; }),
                 released.end());
  std::vector<PlanOp> ops;
  std::string error;
  if (!plan_target(this->schedule_, this->target_, ops, error)) {
    this->target_done_(ScheduleWriteResult::FAILED, error);
    return false;
  }
  if (ops.empty()) {
    this->target_done_(this->target_wrote_ ? ScheduleWriteResult::OK : ScheduleWriteResult::UNCHANGED, "");
    return false;
  }
  if (this->target_rounds_ >= MAX_PLAN_ROUNDS) {
    this->target_done_(ScheduleWriteResult::FAILED,
                       (this->round_clean_ ? "differs after writing: " : "interrupted, not written: ") + describe_ops(ops));
    return false;
  }
  this->target_rounds_++;
  this->round_clean_ = true;
  this->target_wrote_ = true;
  for (auto &op : ops)
    if (op.kind == PlanOp::REMOVE_DAY && std::find(released.begin(), released.end(), op.id) == released.end())
      released.push_back(op.id);
  this->plan_ops_.assign(ops.begin(), ops.end());
  this->new_ids_.assign(ops.size(), NEW_PLAN_ID);
  this->log_('I', "Schedule change: %u steps (round %u)", (unsigned) ops.size(), (unsigned) this->target_rounds_);
  return true;
}

void Session::plan_write_done_(const proto::Frame *frame, bool accepted) {
  PlanOp op = this->plan_ops_.front();
  this->plan_ops_.pop_front();
  static const char *const KINDS[] = {"day plan write", "day plan add", "day plan removal",
                                      "week plan write", "week plan add", "week plan removal"};
  const char *what = KINDS[op.kind];
  // Whatever happens, the next session reads the schedule: it checks the change, or shows what was written.
  this->need_readback_ = true;
  this->schedule_due_ = true;
  if (frame != nullptr && !accepted) {
    std::string failure = std::string(what) + " rejected (result " + std::to_string(frame->result) + ")";
    if (!this->plan_ops_.empty())
      failure += "; " + std::to_string(this->plan_ops_.size()) + " later step(s) not written";
    this->target_done_(ScheduleWriteResult::FAILED, failure);
    this->next_step_();
    return;
  }
  if (frame == nullptr) {
    this->plan_round_interrupted_(std::string("no confirmation for the ") + what);
    this->next_step_();
    return;
  }
  if (op.kind == PlanOp::ADD_DAY) {
    proto::DayPlan added;
    if (!proto::parse_day_plan(frame->body, frame->body_len, added) || added.id == NEW_PLAN_ID) {
      this->plan_round_interrupted_("no id for the added day plan");
      this->next_step_();
      return;
    }
    this->log_('I', "Day plan added with id %u", (unsigned) added.id);
    if (op.slot >= 0 && (size_t) op.slot < this->new_ids_.size())
      this->new_ids_[op.slot] = added.id;
    op.day.id = added.id;
    if (this->target_.kind == ScheduleTarget::ADD_DAY) {
      this->target_.kind = ScheduleTarget::DAY_PLAN;  // from now on: that plan with this content
      this->target_.day.id = added.id;
      this->add_sent_ = false;
    }
    if (!same_content(added, op.day) || added.name != op.day.name) {
      // Old firmware stores an empty plan on add (the app's U0.2.7 workaround): write the content too.
      PlanOp set;
      set.kind = PlanOp::SET_DAY;
      set.day = op.day;
      this->plan_ops_.push_front(set);
    }
  } else if (op.kind == PlanOp::ADD_WEEK) {
    proto::WeekPlan added;
    if (!proto::parse_week_plan(frame->body, frame->body_len, added) || added.id == NEW_PLAN_ID) {
      this->plan_round_interrupted_("no id for the added week plan");
      this->next_step_();
      return;
    }
    this->log_('I', "Week plan added with id %u", (unsigned) added.id);
    if (this->target_.kind == ScheduleTarget::ADD_WEEK) {
      this->target_.kind = ScheduleTarget::WEEK_PLAN;
      this->target_.week.id = added.id;
      this->add_sent_ = false;
    }
  }
  this->plan_.push_front(Step::SET_PLANS);
  this->next_step_();
}

void Session::plan_round_interrupted_(const std::string &why) {
  // Later steps may depend on this one (an add's id, the week write before a removal): stop, and plan the rest
  // again from a fresh read in the next session.
  this->log_('W', "Schedule change interrupted (%s), continuing from a fresh read", why.c_str());
  this->round_clean_ = false;
  this->plan_ops_.clear();
  this->schedule_due_ = true;
}

void Session::target_done_(ScheduleWriteResult result, const std::string &detail) {
  this->has_target_ = false;
  this->target_ = ScheduleTarget{};
  this->plan_ops_.clear();
  this->add_sent_ = false;
  this->target_rounds_ = 0;
  if (result == ScheduleWriteResult::OK)
    this->log_('I', "Schedule written and verified");
  else if (result == ScheduleWriteResult::UNCHANGED)
    this->log_('I', "Schedule already as requested");
  else
    this->log_('W', "Schedule change failed: %s", detail.c_str());
  this->listener_.on_schedule_written(result, detail);
}

void Session::time_done_() {
  this->time_due_ = false;
  this->time_done_at_ = this->clock_.millis();
}

bool Session::next_plan_request_() {
  if (this->plan_requests_.empty())
    return false;
  auto [op, id] = this->plan_requests_.front();
  this->plan_requests_.pop_front();
  uint32_t confirm = op == proto::OP_GET_DAY_PLAN_REQUEST ? proto::OP_GET_DAY_PLAN_CONFIRM : proto::OP_GET_WEEK_PLAN_CONFIRM;
  this->send_(proto::build_get_plan(op, this->reference_++, id), confirm, PLAN_TIMEOUT_MS);
  return true;
}

void Session::handle_rooms_(const proto::Frame &frame) {
  if (frame.result == proto::RESULT_NOT_PAIRED) {
    this->finish_(BridgeStatus::NOT_PAIRED);
    return;
  }
  if (frame.result != proto::RESULT_OK) {
    this->finish_(BridgeStatus::ERROR, "Error " + std::to_string(frame.result));
    return;
  }
  std::vector<proto::Room> rooms;
  if (!proto::parse_rooms(frame.body, frame.body_len, rooms) || rooms.empty()) {
    this->finish_(BridgeStatus::ERROR, "No room data");
    return;
  }
  this->apply_room_(rooms[std::min<size_t>(this->config_.room_index, rooms.size() - 1)]);
  // Every plan reads the run state before the rooms, so a thermostat target goes to the mode active now (or, if that
  // read failed, by the setpoints just read).
  if (this->pending_active_target_ >= 0)
    this->resolve_active_target_(this->run_state_fresh_);

  // Setpoint writes, which begin_() puts after this read, and writes postponed because the current setpoints were
  // unknown (e.g. right after boot) run now.
  if (this->writes_pending_() && !this->wrote_ && !this->retried_writes_) {
    this->retried_writes_ = true;  // once per session, in case the setpoints stay incomplete
    this->plan_.clear();
    this->plan_writes_();
    this->plan_.push_back(Step::GET_ROOMS);  // ends the session and schedules the read-back
  }
  this->next_step_();
}

void Session::apply_room_(const proto::Room &room) {
  this->room_ = room;
  this->has_room_ = true;
  const auto &r = this->room_;
  if (r.overheat_alarm)
    this->log_('W', "Heater reports overheating");
  this->log_('I', "Room '%s': temp %.1f °C, target %.1f °C, setpoints %.1f/%.1f/%.1f, window %s", r.name.c_str(),
             r.measured_temperature, r.target_temperature, r.setpoints[0] / 10.0f, r.setpoints[1] / 10.0f,
             r.setpoints[2] / 10.0f, r.open_window_detected ? "open" : "closed");
  this->last_reading_ms_ = this->clock_.millis();
  if (this->last_reading_ms_ == 0)
    this->last_reading_ms_ = 1;

  this->timer_end_ = r.preheat_end != 0 ? r.preheat_end : r.booster_end;
  uint64_t now = this->clock_.unix_now();
  if (now != 0 && this->timer_end_ > now) {
    this->log_('I', "Heating timer: %u min left", (unsigned) ((this->timer_end_ - now + 59) / 60));
    this->timer_until_ms_ = 0;  // the remote reports it, no local countdown needed
  } else if (now != 0 && this->timer_until_ms_ != 0) {
    // A timer started from here but the remote reports none running: allow one read-back for it to show up,
    // then drop the local countdown (stopped on the remote, or never started).
    if (this->timer_checked_)
      this->timer_until_ms_ = 0;
    this->timer_checked_ = true;
  }
  this->room_errors_ = r.device_errors;
  this->listener_.on_room(this->room_);
  this->publish_errors_();
}

void Session::handle_run_state_(const proto::Frame &frame) {
  proto::RunState rs;
  if (frame.result == proto::RESULT_OK && proto::parse_run_state(frame.body, frame.body_len, rs)) {
    this->temp_mode_ = rs.temp_mode;
    this->run_state_ = rs;
    this->has_run_state_ = true;
    this->run_state_fresh_ = true;
    this->run_state_errors_ = rs.device_errors;
    this->listener_.on_run_state(rs);
  } else {
    this->log_('W', "GetRunState failed (result=%u)", (unsigned) frame.result);
    this->run_state_errors_.clear();
  }
  this->next_step_();
}

void Session::publish_errors_() {
  std::vector<uint32_t> codes;
  const std::vector<uint32_t> *lists[] = {&this->room_errors_, &this->run_state_errors_};
  for (const auto *list : lists)
    for (uint32_t code : *list)
      if (code != proto::DEVICE_ERROR_NONE && std::find(codes.begin(), codes.end(), code) == codes.end())
        codes.push_back(code);
  this->listener_.on_errors(codes);
}

// ── Ending ───────────────────────────────────────────────────────────────────

void Session::note_result_(BridgeStatus status, const std::string &detail) {
  // A pairing request that never reached the remote is dropped, so it can't fire later unattended.
  if (this->pair_requested_ && !this->started_) {
    this->pair_requested_ = false;
    this->set_status_(BridgeStatus::PAIRING_FAILED,
                      std::string("Pairing failed: ") + (detail.empty() ? bridge_status_text(status) : detail));
    status = BridgeStatus::PAIRING_FAILED;
    this->retries_ = 0;
    return;
  }
  // Connection problems are retried soon; finished polls, writes and pairing outcomes are not.
  bool retry = status == BridgeStatus::UNREACHABLE || status == BridgeStatus::CONNECT_FAILED ||
               status == BridgeStatus::DISCONNECTED || status == BridgeStatus::TIMEOUT || status == BridgeStatus::ERROR;
  if (retry)
    this->stats_.failures++;
  if (!retry) {
    this->retries_ = 0;
    this->set_status_(status, detail);
  } else if (this->retries_ < this->config_.max_retries) {
    // A single failed connect is normal (about 1 in 7 on 2026-10-14, the retry worked every time): log it, but
    // show the last settled status until the retries are used up, so Home Assistant and the display don't flicker.
    this->retries_++;
    this->log_('W', "%s; retry %u/%u in %u s", detail.empty() ? bridge_status_text(status) : detail.c_str(),
               this->retries_, this->config_.max_retries, (unsigned) (this->config_.retry_delay_ms / 1000));
    this->set_status_(this->settled_status_, this->settled_text_);
    this->schedule_start_(this->config_.retry_delay_ms);
  } else {
    this->set_status_(status, detail);
  }
}

void Session::finish_(BridgeStatus status, const std::string &detail) {
  this->note_result_(status, detail);
  this->plan_.clear();
  this->rx_.clear();
  this->read_pending_ = false;
  this->transport_.disconnect();
  if (this->transport_.idle()) {
    this->end_();
    return;
  }
  this->step_ = Step::CLOSING;
  this->deadline_ = this->clock_.millis() + CLOSE_TIMEOUT_MS;
}

void Session::end_() {
  if (!this->plan_ops_.empty())
    this->plan_round_interrupted_("session ended");  // e.g. the link dropped before a confirm
  this->ended_at_ = this->clock_.millis();
  this->ended_once_ = true;
  if (this->started_) {
    this->started_ = false;
    this->stats_.last_connected_ms = this->clock_.millis() - this->connected_at_;
    this->stats_.total_connected_ms += this->stats_.last_connected_ms;
    this->log_('D', "Session: %u ms connected (%u connections since boot)", (unsigned) this->stats_.last_connected_ms,
               (unsigned) this->stats_.connections);
    this->listener_.on_session_end(this->stats_);
  }
  this->active_ = false;
  this->step_ = Step::NONE;
  this->plan_.clear();
  this->rx_.clear();
  this->read_pending_ = false;
  this->sent_ = false;
}

}  // namespace esphome::zehnder_wivar
