#include "zehnder_wivar.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <algorithm>

#ifdef USE_TIME_TIMEZONE
#include "esphome/components/time/posix_tz.h"
#endif

#ifdef USE_ESP32

namespace esphome::zehnder_wivar {

static const char *const TAG = "zehnder_wivar";

static const espbt::ESPBTUUID SERVICE_UUID = espbt::ESPBTUUID::from_raw("2456e1b9-26e2-8f83-e744-f34f01e9d701");
static const espbt::ESPBTUUID WRITE_UUID = espbt::ESPBTUUID::from_raw("2456e1b9-26e2-8f83-e744-f34f01e9d702");
static const espbt::ESPBTUUID READ_UUID = espbt::ESPBTUUID::from_raw("2456e1b9-26e2-8f83-e744-f34f01e9d703");

static const size_t BLE_CHUNK = 20;

// ── Entities ─────────────────────────────────────────────────────────────────

void WivarSetpointNumber::control(float value) {
  this->parent_->set_setpoint(this->mode_, value);
  this->publish_state(value);
}

climate::ClimateTraits WivarClimate::traits() {
  climate::ClimateTraits traits;
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE);
  traits.set_supported_modes({climate::CLIMATE_MODE_HEAT});
  traits.set_visual_min_temperature(this->parent_->min_setpoint());
  traits.set_visual_max_temperature(this->parent_->max_setpoint());
  traits.set_visual_target_temperature_step(0.5f);
  traits.set_visual_current_temperature_step(0.1f);
  return traits;
}

void WivarClimate::control(const climate::ClimateCall &call) {
  if (call.get_target_temperature().has_value()) {
    float target = *call.get_target_temperature();
    this->parent_->set_active_target(target);
    this->target_temperature = target;
    this->publish_state();
  }
}

void WivarModeSelect::control(size_t index) {
  if (index > static_cast<size_t>(RunModeChoice::ANTIFREEZE))
    return;
  this->parent_->set_run_mode(static_cast<RunModeChoice>(index));
  this->publish_state(index);
}

// ── Component ────────────────────────────────────────────────────────────────

void ZehnderWivar::setup() {
  this->session_.config().want_version =
      this->firmware_text_sensor_ != nullptr || this->firmware_untested_binary_sensor_ != nullptr;
  this->session_.config().want_schedule = this->schedule_text_sensor_ != nullptr ||
                                          this->schedule_today_text_sensor_ != nullptr ||
                                          this->next_switch_text_sensor_ != nullptr;
  this->on_status(BridgeStatus::WAITING, bridge_status_text(BridgeStatus::WAITING));
  if (this->next_switch_text_sensor_ != nullptr || this->schedule_today_text_sensor_ != nullptr)
    this->set_interval("schedule", 60000, [this]() { this->publish_schedule_(); });
}

void ZehnderWivar::dump_config() {
  ESP_LOGCONFIG(TAG, "Zehnder Wivar:");
  ESP_LOGCONFIG(TAG, "  Nickname: %s", this->session_.config().nickname.c_str());
  ESP_LOGCONFIG(TAG, "  Room index: %u", this->session_.config().room_index);
  ESP_LOGCONFIG(TAG, "  Setpoint range: %.1f-%.1f °C", this->min_setpoint_, this->max_setpoint_);
  ESP_LOGCONFIG(TAG, "  Connect timeout: %u s, retries: %u every %u s, stale after: %u s",
                (unsigned) (this->session_.config().connect_timeout_ms / 1000),
                (unsigned) this->session_.config().max_retries,
                (unsigned) (this->session_.config().retry_delay_ms / 1000), (unsigned) this->stale_after_s_);
  LOG_UPDATE_INTERVAL(this);
  LOG_SENSOR("  ", "Temperature", this->temperature_sensor_);
  LOG_SENSOR("  ", "Target temperature", this->target_temperature_sensor_);
  LOG_SENSOR("  ", "Signal strength", this->signal_strength_sensor_);
}

void ZehnderWivar::update() {
  if (this->has_reading() && this->reading_age_s() > this->stale_after_s_ && !std::isnan(this->temperature_))
    this->invalidate_();
  this->session_.poll();
}

void ZehnderWivar::loop() { this->session_.loop(); }

int32_t ZehnderWivar::to_tenths_(float celsius) const {
  return lroundf(clamp(celsius, this->min_setpoint_, this->max_setpoint_) * 10.0f);
}

void ZehnderWivar::set_setpoint(uint8_t mode, float celsius) {
  if (!std::isnan(celsius))
    this->session_.set_setpoint(mode, this->to_tenths_(celsius));
}

void ZehnderWivar::set_active_target(float celsius) {
  if (!std::isnan(celsius))
    this->session_.set_active_target(this->to_tenths_(celsius));
}

// ── BLE events → session ─────────────────────────────────────────────────────

void ZehnderWivar::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                       esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_OPEN_EVT:
      this->parent()->set_auto_connect(false);
      if (param->open.status != ESP_GATT_OK)
        this->session_.on_connect_failed();
      break;

    case ESP_GATTC_SEARCH_CMPL_EVT: {
      auto *wr = this->parent()->get_characteristic(SERVICE_UUID, WRITE_UUID);
      auto *rd = this->parent()->get_characteristic(SERVICE_UUID, READ_UUID);
      if (wr == nullptr || rd == nullptr) {
        ESP_LOGE(TAG, "Wivar GATT service not found");
        this->session_.on_link_error("Service not found");
        break;
      }
      this->write_handle_ = wr->handle;
      this->read_handle_ = rd->handle;
      // BLEClientBase writes the CCCD once registration completes (ESP_GATTC_WRITE_DESCR_EVT follows).
      if (this->parent()->register_for_notify(this->read_handle_) != ESP_OK)
        this->session_.on_link_error("Notify registration failed");
      break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
      // Handles are cached, so the parent may release its GATT cache.
      this->node_state = espbt::ClientState::ESTABLISHED;
      break;

    case ESP_GATTC_WRITE_DESCR_EVT:
      this->session_.on_connected();
      break;

    case ESP_GATTC_WRITE_CHAR_EVT:
      if (param->write.handle != this->write_handle_)
        break;
      if (param->write.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Write failed, status=%d", param->write.status);
        this->tx_.clear();
        this->session_.on_send_failed();
      } else if (this->tx_offset_ < this->tx_.size()) {
        this->write_next_chunk_();
      } else if (!this->tx_.empty()) {
        this->tx_.clear();
        this->session_.on_sent();  // ack of the last chunk
      }
      break;

    case ESP_GATTC_READ_CHAR_EVT:
      // The read value is stale; the actual data arrives as a notification.
      if (param->read.handle == this->read_handle_)
        this->session_.on_read_done();
      break;

    case ESP_GATTC_NOTIFY_EVT:
      if (param->notify.handle == this->read_handle_)
        this->session_.on_data(param->notify.value, param->notify.value_len);
      break;

    case ESP_GATTC_DISCONNECT_EVT:
    case ESP_GATTC_CLOSE_EVT:
      this->parent()->set_auto_connect(false);
      this->tx_.clear();
      this->node_state = espbt::ClientState::IDLE;
      this->session_.on_disconnected();
      break;

    default:
      break;
  }
}

// ── Transport ────────────────────────────────────────────────────────────────

void ZehnderWivar::connect() {
  // The remote advertises only every ~5-8 s, which a blind connect often misses. Scan instead and let the
  // tracker connect as soon as an advertisement arrives (auto_connect is only on while a session connects).
  this->parent()->set_auto_connect(true);
  if (!espbt::global_esp32_ble_tracker->scan_running())
    espbt::global_esp32_ble_tracker->start_scan();
}

void ZehnderWivar::send(const std::vector<uint8_t> &frame, bool secret) {
  if (secret) {
    ESP_LOGV(TAG, "TX %u bytes (login or pairing, contains the app UUID, not shown)", (unsigned) frame.size());
  } else {
    ESP_LOGV(TAG, "TX %s", format_hex(frame).c_str());
  }
  this->tx_ = frame;
  this->tx_offset_ = 0;
  this->write_next_chunk_();
}

void ZehnderWivar::write_next_chunk_() {
  size_t n = std::min(BLE_CHUNK, this->tx_.size() - this->tx_offset_);
  auto err = esp_ble_gattc_write_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                      this->write_handle_, n, this->tx_.data() + this->tx_offset_,
                                      ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "esp_ble_gattc_write_char failed: %d", err);
    this->tx_.clear();
    this->session_.on_send_failed();
    return;
  }
  this->tx_offset_ += n;
}

void ZehnderWivar::poke() {
  if (esp_ble_gattc_read_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(), this->read_handle_,
                              ESP_GATT_AUTH_REQ_NONE) != ESP_OK)
    this->session_.on_read_done();  // nothing outstanding; the session pokes again later
}

void ZehnderWivar::disconnect() {
  this->parent()->set_auto_connect(false);
  this->tx_.clear();
  if (this->parent()->state() != espbt::ClientState::IDLE)
    this->parent()->disconnect();
}

bool ZehnderWivar::idle() const { return this->parent_->state() == espbt::ClientState::IDLE; }

// ── Clock ────────────────────────────────────────────────────────────────────

uint32_t ZehnderWivar::millis() const { return esphome::millis(); }

uint64_t ZehnderWivar::unix_now() const {
  if (this->time_ == nullptr)
    return 0;
  auto now = this->time_->now();
  return now.is_valid() ? (uint64_t) now.timestamp : 0;
}

uint32_t ZehnderWivar::random_u32() { return random_uint32(); }

bool ZehnderWivar::dst_info(uint64_t utc, bool &summer, std::vector<proto::DstSwitch> &switches) {
#ifdef USE_TIME_TIMEZONE
  // Like the app: the current daylight saving and the coming switches, as local wall-clock times.
  const auto &tz = time::get_global_tz();
  summer = tz.has_dst() && time::is_in_dst((time_t) utc, tz);
  switches.clear();
  if (tz.has_dst())
    switches = proto::find_dst_switches(
        (int64_t) utc,
        [](int64_t t, const void *ctx) {
          return time::is_in_dst((time_t) t, *static_cast<const time::ParsedTimezone *>(ctx));
        },
        &tz, tz.std_offset_seconds, tz.dst_offset_seconds);
  return true;
#else
  return false;
#endif
}

// ── Session results → Home Assistant ─────────────────────────────────────────

void ZehnderWivar::on_status(BridgeStatus status, const std::string &text) {
  if (this->status_text_sensor_ != nullptr)
    this->status_text_sensor_->publish_state(text);
}

void ZehnderWivar::on_room(const proto::Room &room) {
  this->temperature_ = room.has_measured_temperature ? room.measured_temperature : NAN;
  this->target_temperature_ = room.has_target_temperature ? room.target_temperature : NAN;
  this->humidity_ = room.has_humidity ? room.humidity : NAN;
  this->valve_ = room.has_valve ? room.valve : NAN;
  this->window_open_ = room.open_window_detected;
  // Not reported by the Wivar so far; absent counts as not overheated (as in the app).
  this->overheated_ = room.has_overheat_alarm && room.overheat_alarm;
  this->room_name_ = room.name;

  if (this->temperature_sensor_ != nullptr)
    this->temperature_sensor_->publish_state(this->temperature_);
  if (this->target_temperature_sensor_ != nullptr)
    this->target_temperature_sensor_->publish_state(this->target_temperature_);
  if (this->humidity_sensor_ != nullptr)
    this->humidity_sensor_->publish_state(this->humidity_);
  if (this->valve_sensor_ != nullptr)
    this->valve_sensor_->publish_state(this->valve_);
  if (this->signal_strength_sensor_ != nullptr && room.has_signal_strength)
    this->signal_strength_sensor_->publish_state(room.signal_strength);
  if (this->surface_temperature_sensor_ != nullptr && room.has_fluid_temperature)
    this->surface_temperature_sensor_->publish_state(room.fluid_temperature);
  if (this->heating_binary_sensor_ != nullptr && room.has_valve)
    this->heating_binary_sensor_->publish_state(room.valve > 0);
  if (this->window_open_binary_sensor_ != nullptr)
    this->window_open_binary_sensor_->publish_state(room.open_window_detected);
  if (this->overheated_binary_sensor_ != nullptr)
    this->overheated_binary_sensor_->publish_state(this->overheated_);
  if (this->room_name_text_sensor_ != nullptr)
    this->room_name_text_sensor_->publish_state(room.name);
  for (uint8_t mode = 0; mode < proto::NUM_SETPOINTS; mode++) {
    // Don't overwrite a value the user just set and that is still waiting to be written.
    if (this->setpoint_numbers_[mode] != nullptr && room.setpoints[mode] >= 0 &&
        this->session_.pending_setpoint(mode) < 0)
      this->setpoint_numbers_[mode]->publish_state(room.setpoints[mode] / 10.0f);
  }
  this->publish_climate_();
}

void ZehnderWivar::on_run_state(const proto::RunState &rs) {
  if (this->mode_select_ != nullptr && this->session_.pending_run_mode() < 0) {
    int index = -1;
    if (rs.run_mode == proto::RUN_MODE_AUTOMATIC)
      index = static_cast<int>(RunModeChoice::AUTOMATIC);
    else if (rs.run_mode == proto::RUN_MODE_MANUAL)
      index = static_cast<int>(RunModeChoice::MANUAL);
    else if (rs.run_mode == proto::RUN_MODE_ANTIFREEZE)
      index = static_cast<int>(RunModeChoice::ANTIFREEZE);
    if (index >= 0)
      this->mode_select_->publish_state((size_t) index);
  }
  this->publish_schedule_();
  this->run_mode_ = rs.standby ? "Standby" : str_sprintf("%s / %s", proto::run_mode_name(rs.run_mode),
                                                        proto::temp_mode_name(rs.temp_mode));
  if (this->run_mode_text_sensor_ != nullptr)
    this->run_mode_text_sensor_->publish_state(this->run_mode_);
}

void ZehnderWivar::on_errors(const std::vector<uint32_t> &codes) {
  std::string text;
  for (uint32_t code : codes) {
    if (!text.empty())
      text += ", ";
    text += proto::device_error_name(code);
  }
  if (text.empty())
    text = "None";
  else
    ESP_LOGW(TAG, "Remote reports: %s", text.c_str());
  bool battery_low = std::find(codes.begin(), codes.end(), proto::DEVICE_ERROR_BATTERY_LOW) != codes.end();
  if (this->battery_low_binary_sensor_ != nullptr)
    this->battery_low_binary_sensor_->publish_state(battery_low);
  if (this->error_text_sensor_ != nullptr)
    this->error_text_sensor_->publish_state(text);
}

void ZehnderWivar::on_version(uint32_t version, bool tested) {
  if (this->firmware_text_sensor_ != nullptr)
    this->firmware_text_sensor_->publish_state(str_sprintf(
        "%u.%u.%u (0x%06X)%s", (unsigned) ((version >> 16) & 0xFF), (unsigned) ((version >> 8) & 0xFF),
        (unsigned) (version & 0xFF), (unsigned) version, tested ? "" : " untested"));
  if (this->firmware_untested_binary_sensor_ != nullptr)
    this->firmware_untested_binary_sensor_->publish_state(!tested);
}

void ZehnderWivar::on_session_end(const SessionStats &stats) {
  if (this->connections_sensor_ != nullptr)
    this->connections_sensor_->publish_state(stats.connections);
  if (this->connected_time_sensor_ != nullptr)
    this->connected_time_sensor_->publish_state(stats.last_connected_ms / 1000.0f);
}

void ZehnderWivar::on_schedule(const Schedule & /*schedule*/) { this->publish_schedule_(); }

bool ZehnderWivar::set_schedule(const std::string &text) {
  std::string error = this->session_.set_schedule(text);
  if (!error.empty()) {
    ESP_LOGW(TAG, "Schedule not changed: %s", error.c_str());
    if (this->schedule_write_text_sensor_ != nullptr)
      this->schedule_write_text_sensor_->publish_state("Refused: " + error);
    return false;
  }
  if (this->schedule_write_text_sensor_ != nullptr && this->session_.plan_writes_pending())
    this->schedule_write_text_sensor_->publish_state("Writing...");
  return true;
}

void ZehnderWivar::on_schedule_written(ScheduleWriteResult result, const std::string &detail) {
  if (this->schedule_write_text_sensor_ == nullptr)
    return;
  std::string text = result == ScheduleWriteResult::OK          ? std::string("OK")
                     : result == ScheduleWriteResult::UNCHANGED ? std::string("Unchanged")
                                                                : "Failed: " + detail;
  if (text.size() > 255)
    text = text.substr(0, 252) + "...";
  this->schedule_write_text_sensor_->publish_state(text);
}

void ZehnderWivar::on_clock_offset(int64_t seconds) {
  if (this->clock_offset_sensor_ != nullptr)
    this->clock_offset_sensor_->publish_state(seconds);
}

void ZehnderWivar::publish_schedule_() {
  if (!this->session_.has_schedule() || !this->session_.has_run_state())
    return;
  const Schedule &schedule = this->session_.schedule();
  const proto::RunState &rs = this->session_.run_state();
  if (this->schedule_text_sensor_ != nullptr) {
    std::string week = describe_week(schedule, rs);
    if (week.size() > 255)  // Home Assistant's state limit
      week = week.substr(0, 252) + "...";
    if (week != this->schedule_text_sensor_->state)
      this->schedule_text_sensor_->publish_state(week);
  }
  if (this->time_ == nullptr)
    return;
  auto now = this->time_->now();
  if (!now.is_valid())
    return;
  int weekday = (now.day_of_week + 5) % 7;  // ESPTime: 1 = Sunday; here 0 = Monday
  uint32_t now_s = now.hour * 3600 + now.minute * 60 + now.second;
  if (this->schedule_today_text_sensor_ != nullptr) {
    const proto::DayPlan *today = today_plan(schedule, rs, weekday);
    std::string text = today != nullptr ? proto::describe_day_plan(*today) : "-";
    if (text != this->schedule_today_text_sensor_->state)
      this->schedule_today_text_sensor_->publish_state(text);
  }
  if (this->next_switch_text_sensor_ != nullptr) {
    std::string text = describe_next_switch(schedule, rs, weekday, now_s);
    if (text != this->next_switch_text_sensor_->state)
      this->next_switch_text_sensor_->publish_state(text);
  }
}

void ZehnderWivar::on_log(char level, const std::string &message) {
  switch (level) {
    case 'E':
      ESP_LOGE(TAG, "%s", message.c_str());
      break;
    case 'W':
      ESP_LOGW(TAG, "%s", message.c_str());
      break;
    case 'I':
      ESP_LOGI(TAG, "%s", message.c_str());
      break;
    case 'D':
      ESP_LOGD(TAG, "%s", message.c_str());
      break;
    default:
      ESP_LOGV(TAG, "%s", message.c_str());
      break;
  }
}

void ZehnderWivar::publish_climate_() {
  if (this->climate_ == nullptr)
    return;
  this->climate_->current_temperature = this->temperature_;
  this->climate_->target_temperature = this->target_temperature_;
  this->climate_->mode = climate::CLIMATE_MODE_HEAT;
  this->climate_->publish_state();
}

void ZehnderWivar::invalidate_() {
  ESP_LOGW(TAG, "Last reading is older than %u s, marking stale", (unsigned) this->stale_after_s_);
  this->temperature_ = this->target_temperature_ = this->humidity_ = this->valve_ = NAN;
  if (this->temperature_sensor_ != nullptr)
    this->temperature_sensor_->publish_state(NAN);
  if (this->target_temperature_sensor_ != nullptr)
    this->target_temperature_sensor_->publish_state(NAN);
  if (this->humidity_sensor_ != nullptr)
    this->humidity_sensor_->publish_state(NAN);
  if (this->valve_sensor_ != nullptr)
    this->valve_sensor_->publish_state(NAN);
  this->publish_climate_();
}

}  // namespace esphome::zehnder_wivar

#endif
