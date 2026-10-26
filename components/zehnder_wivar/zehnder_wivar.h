#pragma once

#include "esphome/core/component.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/number/number.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/select/select.h"
#include "esphome/components/time/real_time_clock.h"

#ifdef USE_ESP32

#include <esp_gattc_api.h>
#include <string>
#include <vector>

#include "wivar_proto.h"
#include "wivar_session.h"

namespace esphome::zehnder_wivar {

namespace espbt = esphome::esp32_ble_tracker;

class ZehnderWivar;

/// Writable setpoint of one temperature mode (Home Awake / Home Asleep / Away).
class WivarSetpointNumber : public number::Number {
 public:
  void set_parent(ZehnderWivar *parent, uint8_t mode) {
    this->parent_ = parent;
    this->mode_ = mode;
  }

 protected:
  void control(float value) override;
  ZehnderWivar *parent_{nullptr};
  uint8_t mode_{0};
};

/// Thermostat: the target temperature is the setpoint of the currently active temperature mode.
class WivarClimate : public climate::Climate {
 public:
  void set_parent(ZehnderWivar *parent) { this->parent_ = parent; }

 protected:
  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;
  ZehnderWivar *parent_{nullptr};
};

/// Run mode: Automatic / Manual Home / Manual Away / Antifreeze (options in RunModeChoice order).
class WivarModeSelect : public select::Select {
 public:
  void set_parent(ZehnderWivar *parent) { this->parent_ = parent; }

 protected:
  void control(size_t index) override;
  ZehnderWivar *parent_{nullptr};
};

/// ESPHome glue around the Wivar session: BLE GATT transport, clock and publishing to Home Assistant.
/// The session logic itself lives in wivar_session.* (plain C++, host-tested).
class ZehnderWivar : public ble_client::BLEClientNode,
                     public PollingComponent,
                     public Transport,
                     public Clock,
                     public SessionListener {
 public:
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;

  // Configuration (from codegen).
  void set_app_uuid(const std::vector<uint8_t> &uuid) {
    std::copy(uuid.begin(), uuid.end(), this->session_.config().app_uuid);
  }
  void set_nickname(const std::string &nickname) { this->session_.config().nickname = nickname; }
  void set_room_index(uint8_t index) { this->session_.config().room_index = index; }
  void set_time(time::RealTimeClock *t) { this->time_ = t; }
  void set_time_sync(bool check, bool sync) {
    this->session_.config().check_time = check;
    this->session_.config().sync_time = sync;
  }
  void set_connect_timeout(uint32_t ms) { this->session_.config().connect_timeout_ms = ms; }
  void set_retry_interval(uint32_t ms) { this->session_.config().retry_delay_ms = ms; }
  void set_max_retries(uint8_t n) { this->session_.config().max_retries = n; }
  void set_stale_after(uint32_t ms) { this->stale_after_s_ = ms / 1000; }
  void set_setpoint_range(float min, float max) {
    this->min_setpoint_ = min;
    this->max_setpoint_ = max;
    this->session_.config().min_setpoint_tenths = lroundf(min * 10.0f);
    this->session_.config().max_setpoint_tenths = lroundf(max * 10.0f);
  }
  float min_setpoint() const { return this->min_setpoint_; }
  float max_setpoint() const { return this->max_setpoint_; }

  void set_temperature_sensor(sensor::Sensor *s) { this->temperature_sensor_ = s; }
  void set_target_temperature_sensor(sensor::Sensor *s) { this->target_temperature_sensor_ = s; }
  void set_humidity_sensor(sensor::Sensor *s) { this->humidity_sensor_ = s; }
  void set_valve_sensor(sensor::Sensor *s) { this->valve_sensor_ = s; }
  void set_signal_strength_sensor(sensor::Sensor *s) { this->signal_strength_sensor_ = s; }
  void set_connections_sensor(sensor::Sensor *s) { this->connections_sensor_ = s; }
  void set_surface_temperature_sensor(sensor::Sensor *s) { this->surface_temperature_sensor_ = s; }
  void set_clock_offset_sensor(sensor::Sensor *s) { this->clock_offset_sensor_ = s; }
  void set_schedule_text_sensor(text_sensor::TextSensor *s) { this->schedule_text_sensor_ = s; }
  void set_schedule_today_text_sensor(text_sensor::TextSensor *s) { this->schedule_today_text_sensor_ = s; }
  void set_next_switch_text_sensor(text_sensor::TextSensor *s) { this->next_switch_text_sensor_ = s; }
  void set_schedule_write_text_sensor(text_sensor::TextSensor *s) { this->schedule_write_text_sensor_ = s; }
  void set_mode_select(WivarModeSelect *s) { this->mode_select_ = s; }
  void set_connected_time_sensor(sensor::Sensor *s) { this->connected_time_sensor_ = s; }
  void set_heating_binary_sensor(binary_sensor::BinarySensor *s) { this->heating_binary_sensor_ = s; }
  void set_window_open_binary_sensor(binary_sensor::BinarySensor *s) { this->window_open_binary_sensor_ = s; }
  void set_battery_low_binary_sensor(binary_sensor::BinarySensor *s) { this->battery_low_binary_sensor_ = s; }
  void set_overheated_binary_sensor(binary_sensor::BinarySensor *s) { this->overheated_binary_sensor_ = s; }
  void set_firmware_untested_binary_sensor(binary_sensor::BinarySensor *s) { this->firmware_untested_binary_sensor_ = s; }
  void set_room_name_text_sensor(text_sensor::TextSensor *s) { this->room_name_text_sensor_ = s; }
  void set_run_mode_text_sensor(text_sensor::TextSensor *s) { this->run_mode_text_sensor_ = s; }
  void set_status_text_sensor(text_sensor::TextSensor *s) { this->status_text_sensor_ = s; }
  void set_firmware_text_sensor(text_sensor::TextSensor *s) { this->firmware_text_sensor_ = s; }
  void set_error_text_sensor(text_sensor::TextSensor *s) { this->error_text_sensor_ = s; }
  void set_setpoint_number(uint8_t mode, WivarSetpointNumber *n) { this->setpoint_numbers_[mode] = n; }
  void set_climate(WivarClimate *c) { this->climate_ = c; }

  // Commands (buttons, knob, HA entities).
  void start_pairing() { this->session_.start_pairing(); }
  /// Poll now, including the schedule.
  void refresh() {
    this->session_.request_schedule();
    this->session_.poll();
  }
  void set_run_mode(RunModeChoice choice) { this->session_.set_run_mode(choice); }
  /// Make the remote's schedule follow `text` (same format as the Schedule sensor). False if refused; the outcome
  /// shows on the Schedule write sensor.
  bool set_schedule(const std::string &text);
  void set_setpoint(uint8_t mode, float celsius);
  void set_active_target(float celsius);
  void start_timer(uint32_t minutes) { this->session_.start_timer(minutes); }
  void stop_timer() { this->session_.start_timer(0); }

  /// The session, for lambdas and tests (e.g. Session::write_day_plan, which has no YAML entity).
  Session &session() { return this->session_; }

  // Accessors for the display lambda. NAN while no valid reading exists.
  float temperature() const { return this->temperature_; }
  float target_temperature() const { return this->target_temperature_; }
  float humidity() const { return this->humidity_; }
  float valve() const { return this->valve_; }
  bool heating() const { return this->valve_ > 0; }
  bool window_open() const { return this->window_open_; }
  bool overheated() const { return this->overheated_; }
  const std::string &room_name() const { return this->room_name_; }
  const std::string &run_mode() const { return this->run_mode_; }
  BridgeStatus status() const { return this->session_.status(); }
  const std::string &status_text() const { return this->session_.status_text(); }
  bool status_is_problem() const { return bridge_status_is_problem(this->session_.status()); }
  bool has_reading() const { return this->session_.has_reading(); }
  uint32_t reading_age_s() const { return this->session_.reading_age_s(); }
  bool pairing() const { return this->session_.pairing(); }
  uint32_t timer_remaining_s() const { return this->session_.timer_remaining_s(); }

  // Transport
  void connect() override;
  void send(const std::vector<uint8_t> &frame, bool secret) override;
  void poke() override;
  void disconnect() override;
  bool idle() const override;

  // Clock
  uint32_t millis() const override;
  uint64_t unix_now() const override;
  uint32_t random_u32() override;
  bool dst_info(uint64_t utc, bool &summer, std::vector<proto::DstSwitch> &switches) override;

  // SessionListener
  void on_status(BridgeStatus status, const std::string &text) override;
  void on_room(const proto::Room &room) override;
  void on_run_state(const proto::RunState &run_state) override;
  void on_errors(const std::vector<uint32_t> &codes) override;
  void on_version(uint32_t version, bool tested) override;
  void on_session_end(const SessionStats &stats) override;
  void on_schedule(const Schedule &schedule) override;
  void on_schedule_written(ScheduleWriteResult result, const std::string &detail) override;
  void on_clock_offset(int64_t seconds) override;
  void on_log(char level, const std::string &message) override;

 protected:
  void write_next_chunk_();
  void invalidate_();
  void publish_climate_();
  void publish_schedule_();
  int32_t to_tenths_(float celsius) const;

  Session session_{*this, *this, *this};
  time::RealTimeClock *time_{nullptr};

  sensor::Sensor *temperature_sensor_{nullptr};
  sensor::Sensor *target_temperature_sensor_{nullptr};
  sensor::Sensor *humidity_sensor_{nullptr};
  sensor::Sensor *valve_sensor_{nullptr};
  sensor::Sensor *signal_strength_sensor_{nullptr};
  sensor::Sensor *connections_sensor_{nullptr};
  sensor::Sensor *surface_temperature_sensor_{nullptr};
  sensor::Sensor *clock_offset_sensor_{nullptr};
  text_sensor::TextSensor *schedule_text_sensor_{nullptr};
  text_sensor::TextSensor *schedule_today_text_sensor_{nullptr};
  text_sensor::TextSensor *next_switch_text_sensor_{nullptr};
  text_sensor::TextSensor *schedule_write_text_sensor_{nullptr};
  WivarModeSelect *mode_select_{nullptr};
  sensor::Sensor *connected_time_sensor_{nullptr};
  binary_sensor::BinarySensor *heating_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *window_open_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *battery_low_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *overheated_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *firmware_untested_binary_sensor_{nullptr};
  text_sensor::TextSensor *room_name_text_sensor_{nullptr};
  text_sensor::TextSensor *run_mode_text_sensor_{nullptr};
  text_sensor::TextSensor *status_text_sensor_{nullptr};
  text_sensor::TextSensor *firmware_text_sensor_{nullptr};
  text_sensor::TextSensor *error_text_sensor_{nullptr};
  WivarSetpointNumber *setpoint_numbers_[proto::NUM_SETPOINTS]{};
  WivarClimate *climate_{nullptr};

  float min_setpoint_{7.0f};
  float max_setpoint_{28.0f};
  uint32_t stale_after_s_{1200};

  // GATT transport.
  uint16_t write_handle_{0};
  uint16_t read_handle_{0};
  std::vector<uint8_t> tx_;
  size_t tx_offset_{0};

  // Last published values (for the display and staleness).
  float temperature_{NAN};
  float target_temperature_{NAN};
  float humidity_{NAN};
  float valve_{NAN};
  bool window_open_{false};
  bool overheated_{false};
  std::string room_name_;
  std::string run_mode_;
};

}  // namespace esphome::zehnder_wivar

#endif
