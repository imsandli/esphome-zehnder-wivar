#pragma once

// Session logic of the Wivar bridge: what to ask the remote in which order, timeouts, retries, write confirmation
// and timer bookkeeping. Plain C++ without ESPHome, so it runs in host tests against a scripted fake remote.
// The ESPHome component provides the BLE transport, the clock and publishes what the listener receives.

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "wivar_proto.h"
#include "wivar_schedule.h"

namespace esphome::zehnder_wivar {

/// Bridge state, published as text on the "BLE status" sensor. Logic (retries, display) uses the enum, never the text.
enum class BridgeStatus : uint8_t {
  STARTING,
  WAITING,
  CONNECTING,
  READING,
  WRITING,
  OK,                  // poll finished
  WRITTEN,             // write finished (read-back scheduled unless the remote already sent the new values)
  PAIRING_CONNECTING,  // pairing requested, connecting
  PAIRING,             // waiting for MODE on the remote
  PAIRED,
  PAIRING_FAILED,      // detail says why
  NOT_PAIRED,
  UNREACHABLE,         // no connection came up
  CONNECT_FAILED,
  DISCONNECTED,        // link dropped mid-session
  TIMEOUT,             // no room data in time
  ERROR,               // detail says what
};

const char *bridge_status_text(BridgeStatus status);
/// True for states the user should notice (connection problems, errors, not paired).
bool bridge_status_is_problem(BridgeStatus status);

/// Remote firmware the protocol was verified against.
static const uint32_t TESTED_REMOTE_FIRMWARE = 0x100411;

/// BLE link to the remote, implemented by the ESPHome component (and by the fake remote in tests).
class Transport {
 public:
  virtual ~Transport() = default;
  /// Start connecting; the transport later calls Session::on_connected() or on_connect_failed()/on_disconnected().
  virtual void connect() = 0;
  /// Send one request frame (the transport chunks it); Session::on_sent() once the last chunk is acknowledged.
  /// `secret`: login and pairing requests, whose body holds the app UUID; the frame mustn't be logged.
  virtual void send(const std::vector<uint8_t> &frame, bool secret) = 0;
  /// Read the reply characteristic: makes the remote push its next chunk.
  virtual void poke() = 0;
  /// Close the link; Session::on_disconnected() follows unless the link is already down.
  virtual void disconnect() = 0;
  /// True when no connection exists or is being set up.
  virtual bool idle() const = 0;
};

class Clock {
 public:
  virtual ~Clock() = default;
  virtual uint32_t millis() const = 0;
  /// Unix time in seconds, 0 while unknown.
  virtual uint64_t unix_now() const = 0;
  /// Random number (pairing nonce).
  virtual uint32_t random_u32() = 0;
  /// For setting the remote's clock: whether summer time applies at `utc`, and the next daylight-saving switches.
  /// False when the local timezone is unknown (then the clock isn't set).
  virtual bool dst_info(uint64_t /*utc*/, bool & /*summer*/, std::vector<proto::DstSwitch> & /*switches*/) {
    return false;
  }
};

struct SessionStats {
  /// Sessions that got connected, since boot.
  uint32_t connections{0};
  /// Connected time of the last session and the sum over all of them, in ms.
  uint32_t last_connected_ms{0};
  uint64_t total_connected_ms{0};
  /// Sessions that ended with a problem (unreachable, timeout, ...), since boot.
  uint32_t failures{0};
};

/// What the run mode select offers. The Wivar's Manual is always Home Awake: it ignores the Away location
/// (tested 2026-10-14, firmware 0x100411).
enum class RunModeChoice : uint8_t { AUTOMATIC, MANUAL, ANTIFREEZE };

/// Outcome of a schedule change.
enum class ScheduleWriteResult : uint8_t {
  OK,         // written, and a read shows the remote as requested
  UNCHANGED,  // a read showed the remote as requested already, nothing written
  FAILED,     // detail says why
};

/// Receives the results of a session.
class SessionListener {
 public:
  virtual ~SessionListener() = default;
  virtual void on_status(BridgeStatus status, const std::string &text) = 0;
  virtual void on_room(const proto::Room &room) = 0;
  virtual void on_run_state(const proto::RunState &run_state) = 0;
  /// Errors from the device record and the run state, NoError and duplicates removed; once per poll.
  virtual void on_errors(const std::vector<uint32_t> &codes) = 0;
  virtual void on_version(uint32_t version, bool tested) = 0;
  /// After every session that got connected: connection count and time, the costs for the remote's batteries.
  virtual void on_session_end(const SessionStats & /*stats*/) {}
  /// The remote's clock minus ours, in seconds, whenever it is read.
  virtual void on_clock_offset(int64_t /*seconds*/) {}
  /// The remote's schedule, after each complete read.
  virtual void on_schedule(const struct Schedule & /*schedule*/) {}
  /// Outcome of a schedule change, once a schedule read shows it done, or when it fails.
  virtual void on_schedule_written(ScheduleWriteResult /*result*/, const std::string & /*detail*/) {}
  /// level: 'E', 'W', 'I', 'D', 'V'
  virtual void on_log(char level, const std::string &message) = 0;
};

struct SessionConfig {
  uint8_t app_uuid[16]{};
  std::string nickname{"ESPHome"};
  uint8_t room_index{0};
  bool want_version{true};
  /// No session starts before this uptime: right after boot the Wi-Fi scan blocks BLE connects.
  uint32_t boot_delay_ms{15000};
  /// Pause after a session before the next one. Measured on 2026-10-14 (Shelly, 41 sessions): 9/13 connects worked
  /// 5-10 s after the previous session, 10/11 after 15-20 s, 6/6 after 30 s; heating made short gaps worse.
  uint32_t session_gap_ms{20000};
  /// Scan + connect limit; the BLE scan must run at least this long.
  uint32_t connect_timeout_ms{40000};
  /// A failed poll is retried after this delay, up to max_retries times, instead of waiting a whole interval.
  uint32_t retry_delay_ms{30000};
  uint8_t max_retries{3};
  /// Setpoint writes are clamped to this range (tenths of °C), within the app's per-mode limits.
  int32_t min_setpoint_tenths{70};
  int32_t max_setpoint_tenths{280};
  /// Read the schedule (only needed when something shows it).
  bool want_schedule{true};
  /// The schedule is read in the first session and then at most this often (it only changes in the app).
  uint32_t schedule_interval_ms{24 * 3600 * 1000};
  /// Read the remote's clock this often, and set it (time and daylight-saving switches, like the app) only when it is
  /// off by more than time_tolerance_s or its daylight-saving data doesn't match. A remote that doesn't answer the
  /// read (the Wivar, firmware 0x100411) is set at this interval without a check, as the app does on every connect.
  bool check_time{true};
  bool sync_time{true};
  uint32_t time_sync_interval_ms{24 * 3600 * 1000};
  uint32_t time_tolerance_s{30};
};

class Session {
 public:
  Session(Transport &transport, Clock &clock, SessionListener &listener, SessionConfig config = {});

  // Commands. Each starts a session as soon as possible (now, or right after the running one).
  void poll();
  void start_pairing();
  /// Setpoint for temp mode 0..2 (Home Awake, Home Asleep, Away) in tenths of °C, 70..280.
  void set_setpoint(uint8_t mode, int32_t tenths);
  /// Change the setpoint of the currently active temperature mode.
  void set_active_target(int32_t tenths);
  /// Start the heating timer for the given minutes; 0 stops it.
  void start_timer(uint32_t minutes);
  /// Switch Automatic / Manual / Antifreeze.
  void set_run_mode(RunModeChoice choice);
  /// Read the schedule in the next session, even if it was read recently.
  void request_schedule() { this->schedule_due_ = true; }
  // Schedule changes. Each needs a schedule read first (checked against it) and returns "" when accepted, else why
  // not. One change at a time: a new one is refused until the previous one has a result (on_schedule_written()).
  // The change is kept as a target: every write session reads the schedule, plans the steps from that read and
  // writes them; the next session reads again and either finds the target reached or continues from there. A step
  // without a confirmation (timeout, dropped link) ends the round and the rest is planned again from a fresh read.
  /// Make the week plan in use follow `text` ("Mo-Fr: 06:20 Home, 06:45 Away; Sa-Su: ..."; see parse_week_text);
  /// adds, rewrites, renames and removes day plans as needed.
  std::string set_schedule(const std::string &text);
  /// Replace an existing day / week plan (same id).
  std::string write_day_plan(const proto::DayPlan &plan);
  std::string write_week_plan(const proto::WeekPlan &plan);
  /// Add a day / week plan (the remote assigns the id); remove one that no week plan uses / that isn't in use. An add
  /// that gets no confirmation is not repeated (it may have happened): it fails with "interrupted".
  std::string add_day_plan(const proto::DayPlan &plan);
  std::string remove_day_plan(uint32_t id);
  std::string add_week_plan(const proto::WeekPlan &plan);
  std::string remove_week_plan(uint32_t id);
  /// A schedule change is waiting for its result.
  bool plan_writes_pending() const { return this->has_target_; }
  /// The week plan in use (todaysWeekplanId, else the only one); -1 if unknown.
  int active_week_plan() const;

  // Transport events.
  void on_connected();
  void on_connect_failed();
  void on_disconnected();
  void on_sent();
  void on_send_failed() { this->on_link_error("Write failed"); }
  /// The link is unusable (e.g. service missing); ends the session with an error.
  void on_link_error(const std::string &detail);
  void on_data(const uint8_t *data, size_t len);
  void on_read_done();

  /// Drive timeouts, read polling and scheduled sessions; call often.
  void loop();

  /// Configuration; change before the first session.
  SessionConfig &config() { return this->config_; }

  // State.
  BridgeStatus status() const { return this->status_; }
  const std::string &status_text() const { return this->status_text_; }
  bool active() const { return this->active_; }
  bool pairing() const { return this->step_ == Step::PAIRING; }
  bool has_reading() const { return this->last_reading_ms_ != 0; }
  uint32_t reading_age_s() const;
  const proto::Room &room() const { return this->room_; }
  int temp_mode() const { return this->temp_mode_; }
  /// Pending (not yet written) setpoint for a mode in tenths, -1 if none.
  int32_t pending_setpoint(uint8_t mode) const { return mode < proto::NUM_SETPOINTS ? this->pending_setpoints_[mode] : -1; }
  /// Seconds left on the running heating timer: the remote's end time, else a timer just started from here.
  uint32_t timer_remaining_s() const;
  const SessionStats &stats() const { return this->stats_; }
  bool has_run_state() const { return this->has_run_state_; }
  const proto::RunState &run_state() const { return this->run_state_; }
  bool has_schedule() const { return this->has_schedule_; }
  const Schedule &schedule() const { return this->schedule_; }
  /// Pending (not yet written) run mode, -1 if none.
  int pending_run_mode() const { return this->pending_run_mode_; }

 protected:
  enum class Step {
    NONE,
    CONNECTING,
    LOGIN,
    VERSION,
    PAIRING,
    SET_SETPOINTS,
    SET_TIMER,
    SET_RUN_STATE,
    SET_PLANS,
    GET_ROOMS,
    GET_RUN_STATE,
    GET_SCHEDULE_IDS,
    GET_PLAN,
    GET_TIME,
    SET_TIME,
    CLOSING
  };

  void log_(char level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
  void set_status_(BridgeStatus status, const std::string &detail = "");
  void request_start_();
  void schedule_start_(uint32_t delay_ms);
  void start_();
  void begin_();
  void next_step_();
  void send_(const std::vector<uint8_t> &frame, uint32_t expect, uint32_t timeout_ms);
  void handle_frame_(const proto::Frame &frame);
  void handle_rooms_(const proto::Frame &frame);
  void apply_room_(const proto::Room &room);
  int32_t clamp_setpoint_(uint8_t mode, int32_t tenths) const;
  void resolve_active_target_(bool mode_known);
  void handle_run_state_(const proto::Frame &frame);
  void handle_plan_(const proto::Frame &frame);
  bool next_plan_request_();
  void step_timed_out_();
  void write_done_(bool accepted, bool with_room = false);
  void publish_errors_();
  void note_result_(BridgeStatus status, const std::string &detail);
  void finish_(BridgeStatus status, const std::string &detail = "");
  void end_();
  bool writes_pending_() const;
  bool setpoints_pending_() const;
  void plan_writes_();

  Transport &transport_;
  Clock &clock_;
  SessionListener &listener_;
  SessionConfig config_;

  BridgeStatus status_{BridgeStatus::STARTING};
  std::string status_text_{"Starting"};
  // The last status that isn't part of a running session (OK, Written, a problem, ...): shown again while a failed
  // connect waits for its retry.
  BridgeStatus settled_status_{BridgeStatus::WAITING};
  std::string settled_text_{"Waiting"};

  // Session flow.
  bool active_{false};
  bool started_{false};  // connected and steps begun
  bool start_requested_{false};
  bool start_scheduled_{false};
  uint32_t start_at_{0};
  std::deque<Step> plan_;
  Step step_{Step::NONE};
  uint32_t expect_{0};
  uint32_t deadline_{0};
  uint32_t reference_{1};
  bool sent_{false};  // last chunk of the current request acknowledged
  bool read_pending_{false};
  uint32_t last_activity_{0};
  std::vector<uint8_t> rx_;
  bool wrote_{false};
  bool need_readback_{false};  // a write in this session whose effect hasn't been read yet
  bool retried_writes_{false};
  bool run_state_fresh_{false};  // temp_mode_ comes from a GetRunState answer in this session
  uint8_t retries_{0};
  uint32_t connected_at_{0};
  uint32_t ended_at_{0};
  bool ended_once_{false};
  SessionStats stats_;

  // Requests.
  bool pair_requested_{false};
  // A command queued while an earlier one is in flight stays pending: write_done_() clears only what was sent.
  int32_t pending_setpoints_[proto::NUM_SETPOINTS]{-1, -1, -1};
  int32_t sent_setpoints_[proto::NUM_SETPOINTS]{-1, -1, -1};     // pending_setpoints_ at the write
  int32_t written_setpoints_[proto::NUM_SETPOINTS]{-1, -1, -1};  // all setpoints the write carried
  int32_t pending_active_target_{-1};  // thermostat target, resolved to a mode once the run state has been read
  int32_t pending_timer_{-1};
  int32_t sent_timer_{-1};
  int pending_run_mode_{-1};  // RunModeChoice
  int sent_run_mode_{-1};
  uint32_t written_run_mode_{0};

  // What the remote reported.
  proto::Room room_;
  bool has_room_{false};
  int temp_mode_{-1};
  bool have_version_{false};
  std::vector<uint32_t> room_errors_;
  std::vector<uint32_t> run_state_errors_;  // from the last GetRunState, empty if it failed
  uint32_t last_reading_ms_{0};
  uint64_t timer_end_{0};
  uint32_t timer_until_ms_{0};
  bool timer_checked_{false};
  proto::RunState run_state_;
  bool has_run_state_{false};

  // Schedule.
  bool schedule_due_{true};
  uint32_t schedule_read_at_{0};
  bool has_schedule_{false};
  Schedule schedule_;
  Schedule schedule_new_;  // being read
  bool schedule_fresh_{false};  // read completely in this session
  std::deque<std::pair<uint32_t, uint32_t>> plan_requests_;  // (request op, id)

  // Schedule change (see set_schedule).
  bool has_target_{false};
  ScheduleTarget target_;
  bool target_in_session_{false};  // the change was pending when this session began: read, plan, write
  bool target_planned_{false};     // planned in this session
  bool target_wrote_{false};       // something was written for it
  bool add_sent_{false};           // ADD_* target: the add went out without a confirmation yet
  bool round_clean_{false};        // every step of the last round was confirmed
  uint8_t target_rounds_{0};       // sessions that wrote for it (or couldn't read the schedule)
  std::deque<PlanOp> plan_ops_;    // this round's steps; dropped when the round ends early
  std::vector<uint32_t> new_ids_;  // ids the remote assigned to added day plans, by slot
  std::string start_target_(const ScheduleTarget &target);
  bool next_plan_write_();
  bool plan_round_();
  void plan_write_done_(const proto::Frame *frame, bool accepted);
  void plan_round_interrupted_(const std::string &why);
  void target_done_(ScheduleWriteResult result, const std::string &detail);

  // Remote clock.
  bool time_due_{true};
  proto::RemoteTime remote_time_;
  bool have_remote_time_{false};   // read in this session
  bool get_time_unsupported_{false};
  int64_t remote_offset_{0};
  uint32_t time_done_at_{0};
  void time_done_();
};

}  // namespace esphome::zehnder_wivar
