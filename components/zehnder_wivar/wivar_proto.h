#pragma once

// Minimal Zehnder Wivar wire protocol: length-prefixed frames carrying
// hand-rolled protobuf; see docs/PROTOCOL.md.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace esphome::zehnder_wivar::proto {

// OperationType values
static const uint32_t OP_SET_ADDRESS_REQUEST = 1;
static const uint32_t OP_VERSION_REQUEST = 18;
static const uint32_t OP_SET_ADDRESS_CONFIRM = 51;
static const uint32_t OP_SCHEDULE_UPDATE_INDICATION = 160;
static const uint32_t OP_GET_DAY_PLAN_REQUEST = 161;
static const uint32_t OP_GET_DAY_PLAN_CONFIRM = 162;
static const uint32_t OP_GET_WEEK_PLAN_REQUEST = 163;
static const uint32_t OP_GET_WEEK_PLAN_CONFIRM = 164;
static const uint32_t OP_REMOVE_DAY_PLAN_REQUEST = 165;
static const uint32_t OP_REMOVE_DAY_PLAN_CONFIRM = 166;
static const uint32_t OP_REMOVE_WEEK_PLAN_REQUEST = 167;
static const uint32_t OP_REMOVE_WEEK_PLAN_CONFIRM = 168;
static const uint32_t OP_ADD_DAY_PLAN_REQUEST = 169;
static const uint32_t OP_ADD_DAY_PLAN_CONFIRM = 170;
static const uint32_t OP_ADD_WEEK_PLAN_REQUEST = 171;
static const uint32_t OP_ADD_WEEK_PLAN_CONFIRM = 172;
static const uint32_t OP_SET_DAY_PLAN_REQUEST = 173;
static const uint32_t OP_SET_DAY_PLAN_CONFIRM = 174;
static const uint32_t OP_SET_WEEK_PLAN_REQUEST = 175;
static const uint32_t OP_SET_WEEK_PLAN_CONFIRM = 176;
static const uint32_t OP_GET_SCHEDULE_IDS_REQUEST = 177;
static const uint32_t OP_GET_SCHEDULE_IDS_CONFIRM = 178;
static const uint32_t OP_VERSION_CONFIRM = 68;
static const uint32_t OP_ROOMS_UPDATE_INDICATION = 196;
static const uint32_t OP_SET_ROOM_VALUE_REQUEST = 205;
static const uint32_t OP_SET_ROOM_VALUE_CONFIRM = 206;
static const uint32_t OP_GET_ROOMS_REQUEST = 194;
static const uint32_t OP_GET_ROOMS_CONFIRM = 195;
static const uint32_t OP_ROOM_VALUE_UPDATE_INDICATION = 207;
static const uint32_t OP_GET_RUN_STATE_REQUEST = 240;
static const uint32_t OP_GET_RUN_STATE_CONFIRM = 241;
static const uint32_t OP_RUN_STATE_UPDATE_INDICATION = 242;
static const uint32_t OP_SET_RUN_STATE_REQUEST = 243;
static const uint32_t OP_SET_RUN_STATE_CONFIRM = 244;
static const uint32_t OP_GET_TIME_REQUEST = 260;
static const uint32_t OP_GET_TIME_CONFIRM = 261;
static const uint32_t OP_SET_TIME_REQUEST = 262;
static const uint32_t OP_SET_TIME_CONFIRM = 263;
static const uint32_t OP_APP_PAIRING_REQUEST = 293;
static const uint32_t OP_APP_PAIRING_CONFIRM = 294;
static const uint32_t OP_SET_DEVICE_FUNCTION_MODE_REQUEST = 334;
static const uint32_t OP_SET_DEVICE_FUNCTION_MODE_CONFIRM = 335;

// GatewayResult values
static const uint32_t RESULT_OK = 0;
static const uint32_t RESULT_NOT_PAIRED = 5;

/// One decoded protobuf field. Varints live in `value`, length-delimited data in `data`/`len`.
struct Field {
  uint32_t number;
  uint8_t wire_type;
  uint64_t value;
  const uint8_t *data;
  size_t len;
};

/// Decode all top-level fields of a protobuf message. Returns false on malformed input.
bool decode_message(const uint8_t *data, size_t len, std::vector<Field> &out);

/// Request frame with an empty body (GetRooms, GetRunState, ...).
std::vector<uint8_t> build_request(uint32_t op_type, uint32_t reference);

/// AppPairingRequest frame.
std::vector<uint8_t> build_pairing_request(uint32_t reference, const uint8_t app_uuid[16], const uint8_t nonce[16],
                                           const std::string &nickname);

/// SetAddressRequest: announces the paired app UUID at the start of a session (the app's "login").
std::vector<uint8_t> build_set_address(uint32_t reference, const uint8_t app_uuid[16]);

/// Number of temperature modes with a setpoint: Home Awake, Home Asleep, Away.
static const int NUM_SETPOINTS = 3;
static const uint8_t TEMP_MODE_ASLEEP = 1;

/// SetRoomValueRequest carrying the per-mode setpoints (tenths of °C). This is how the app changes temperatures.
std::vector<uint8_t> build_set_setpoints(uint32_t reference, uint32_t room_id, const int32_t setpoints[NUM_SETPOINTS]);

/// SetDeviceFunctionModeRequest for the towel preheat timer (what the remote's 1/2/4 h timer keys do).
/// minutes = 0 switches the timer off.
std::vector<uint8_t> build_preheat_timer(uint32_t reference, uint32_t device_id, uint32_t minutes);

/// eRunMode / eHomeAway values.
static const uint32_t RUN_MODE_AUTOMATIC = 1;
static const uint32_t RUN_MODE_MANUAL = 2;
static const uint32_t RUN_MODE_ANTIFREEZE = 3;
static const uint32_t LOCATION_HOME = 1;
static const uint32_t LOCATION_AWAY = 2;

/// SetRunStateRequest { runMode(1), userLocation(2) }: what the app sends to switch Automatic / Manual / Antifreeze.
/// In Manual, the location picks the active temperature (Home Awake or Away).
std::vector<uint8_t> build_set_run_state(uint32_t reference, uint32_t run_mode, uint32_t location);

/// GetDayPlanRequest / GetWeekPlanRequest { id(1) }.
std::vector<uint8_t> build_get_plan(uint32_t op_type, uint32_t reference, uint32_t id);

/// A daylight-saving switch: local wall-clock time of the switch (seconds, as if local time were UTC) and
/// whether summer time applies after it.
struct DstSwitch {
  uint64_t local_time{0};
  bool summer_after{false};
};

/// The next daylight-saving switches after `utc` (at most `max`, within two years), from a "is summer time at t"
/// function; offsets are POSIX style (seconds west of UTC) for standard and summer time.
std::vector<DstSwitch> find_dst_switches(int64_t utc, bool (*in_dst)(int64_t t, const void *ctx), const void *ctx,
                                         int32_t std_offset, int32_t dst_offset, size_t max = 4);

/// SetTimeRequest { utcTime(1), daylightSaving(2): Winter 2 / Summer 3, repeated switchTime(3) { localSwitchTime(1),
/// daylightSaving(2) } }: what the app sends on every connect.
std::vector<uint8_t> build_set_time(uint32_t reference, uint64_t utc, bool summer, const std::vector<DstSwitch> &switches);

/// Total frame size (4-byte header included) once at least 4 bytes are buffered, else 0.
size_t frame_size(const std::vector<uint8_t> &buf);

struct Frame {
  uint32_t op_type{0};
  uint32_t result{0};
  uint32_t reference{0};
  const uint8_t *body{nullptr};
  size_t body_len{0};
};

/// Split a complete frame into its GatewayOperation header and body.
bool parse_frame(const uint8_t *data, size_t len, Frame &out);

struct Room {
  uint32_t id{0};
  std::string name;
  bool has_measured_temperature{false};
  float measured_temperature{0};  // °C
  bool has_target_temperature{false};
  float target_temperature{0};  // °C
  bool has_humidity{false};
  uint32_t humidity{0};  // %
  bool has_valve{false};
  int32_t valve{0};  // TemperatureActuator value, > 0 means heating
  bool open_window_detected{false};
  bool open_window_detection_enabled{false};
  int32_t setpoints[NUM_SETPOINTS]{-1, -1, -1};  // tenths of °C per eTempMode, -1 = not reported
  bool has_signal_strength{false};
  uint32_t signal_strength{0};  // cDevice.signalStrength of the first device
  bool has_device{false};
  uint32_t device_id{0};        // cDevice.id of the first device
  uint64_t preheat_end{0};      // cDevice.towelPreheatFunctionEndDate, unix seconds, 0 = not reported
  uint64_t booster_end{0};      // cRoom.temperatureBoosterFunctionEndDate, unix seconds (where the Wivar reports its timer)
  std::vector<uint32_t> device_errors;  // cDevice.error codes (eDeviceError, 1 = NoError) of all devices
  uint32_t preheat_duration{0};  // cDevice.towelPreheatFunctionSelectedDuration, minutes
  bool has_overheat_alarm{false};
  bool overheat_alarm{false};  // cDevice.overheatAlarm; the app only shows it for the Zenia, never seen from a Wivar
  bool has_fluid_temperature{false};
  uint32_t fluid_temperature{0};  // cDevice.fluidTemperature, °C: heater surface temperature setting (app: 50 or 65)
};

/// Parse a GetRoomsConfirm body.
bool parse_rooms(const uint8_t *body, size_t len, std::vector<Room> &rooms);

struct RunState {
  uint32_t run_mode{0};   // 1 Automatic, 2 Manual, 3 Antifreeze, 4 Off
  uint32_t temp_mode{0};  // 0 Home Awake, 1 Home Asleep, 2 Away, 3 Antifreeze, 4 Override
  uint32_t following_schedule{0};
  bool standby{false};
  bool has_today_dayplan{false};
  uint32_t today_dayplan_id{0};   // cRunstate.todaysDayplanId
  bool has_today_weekplan{false};
  uint32_t today_weekplan_id{0};  // cRunstate.todaysWeekplanId
  std::vector<uint32_t> device_errors;  // cRunstate.deviceErrors (eDeviceError)
};

/// GetTimeConfirm { utcTime(1), localTime(2), daylightSaving(3), repeated switchTime(4) }.
struct RemoteTime {
  uint64_t utc{0};
  uint64_t local{0};
  uint32_t daylight_saving{0};  // 0 not set, 1 none, 2 winter, 3 summer
  std::vector<DstSwitch> switches;
};
bool parse_time(const uint8_t *body, size_t len, RemoteTime &out);

/// Parse the systemVersion from a VersionConfirm body. Returns false if absent.
bool parse_version(const uint8_t *body, size_t len, uint32_t &version);

/// Parse a GetRunStateConfirm body.
bool parse_run_state(const uint8_t *body, size_t len, RunState &out);

// ── Schedule (read only) ──────────────────────────────────────────────────────

struct DayPlanItem {
  uint32_t start_s{0};    // seconds after midnight, remote local time
  uint32_t temp_mode{0};  // eTempMode
};
struct FunctionPlanItem {
  uint32_t start_s{0};
  uint32_t mode{0};  // eFunctionMode, 0 = TowelPreheat
  uint32_t duration_min{0};
  bool has_room{false};  // roomId / deviceId: set by the app when it knows them, kept for a faithful write-back
  uint32_t room_id{0};
  bool has_device{false};
  uint32_t device_id{0};
};
struct DayPlan {
  uint32_t id{0};
  std::string name;
  std::vector<DayPlanItem> items;          // sorted by start time
  std::vector<FunctionPlanItem> functions;  // sorted by start time
};
struct WeekPlan {
  uint32_t id{0};
  std::string name;
  bool has_day[7]{};
  uint32_t day_ids[7]{};  // Monday .. Sunday
};

/// GetScheduleIdsConfirm { repeated dPlanIds(1), repeated wPlanIds(2) }.
bool parse_schedule_ids(const uint8_t *body, size_t len, std::vector<uint32_t> &day_ids, std::vector<uint32_t> &week_ids);
/// GetDayPlanConfirm { dPlan(1): cDayplan { id(1), name(2), dpItem(3) {startTime, tempMode}, functionDpItem(20) } }.
/// startTime is minutes after midnight on the wire; DayPlanItem::start_s is seconds.
bool parse_day_plan(const uint8_t *body, size_t len, DayPlan &out);
/// GetWeekPlanConfirm { wPlan(1): cWeekplan { id(1), name(2), mondayId(3) .. sundayId(9) } }.
bool parse_week_plan(const uint8_t *body, size_t len, WeekPlan &out);
/// "07:00 Home, 22:00 Away, Preheat 06:30 90 min"; temperature modes by name, times as HH:MM.
std::string describe_day_plan(const DayPlan &plan);
/// "Home", "Asleep", "Away", "Antifreeze" (as in describe_day_plan), "?" for others.
const char *short_temp_mode_name(uint32_t mode);

bool same_plan(const DayPlan &a, const DayPlan &b);
bool same_plan(const WeekPlan &a, const WeekPlan &b);

/// SetDayplanRequest { dPlan(1) } / SetWeekplanRequest { wPlan(1) }: what the app sends after editing a plan. The app
/// doesn't lock plans on the Wivar before editing them. Answered by the confirm carrying the plan;
/// possibly a ScheduleUpdateIndication too.
std::vector<uint8_t> build_set_day_plan(uint32_t reference, const DayPlan &plan);
std::vector<uint8_t> build_set_week_plan(uint32_t reference, const WeekPlan &plan);
/// AddDayPlanRequest { dPlan(1) } / AddWeekPlanRequest { wPlan(1) }: the complete plan; the confirm returns it with the
/// id the remote assigned. (The app adds a SetDayplan afterwards only for Wivar firmware U0.2.7.)
std::vector<uint8_t> build_add_day_plan(uint32_t reference, const DayPlan &plan);
std::vector<uint8_t> build_add_week_plan(uint32_t reference, const WeekPlan &plan);
/// RemoveDayPlanRequest { dayplanId(1) } / RemoveWeekPlanRequest { weekplanId(1) }; op is the request type.
std::vector<uint8_t> build_remove_plan(uint32_t op_type, uint32_t reference, uint32_t id);

/// The app's limits for the Wivar: 1-13 entries, the first at 00:00, 5-minute steps, at least 10 minutes apart,
/// Home Awake or Away only; preheat timers 15-240 min in 15-minute steps. Returns "" if valid, else why not.
std::string validate_day_plan(const DayPlan &plan);
/// All seven days set. Whether the day plans exist is up to the caller.
std::string validate_week_plan(const WeekPlan &plan);
static const size_t MAX_DAY_PLAN_ITEMS = 13;

const char *run_mode_name(uint32_t mode);
const char *device_error_name(uint32_t code);
static const uint32_t DEVICE_ERROR_NONE = 1;
static const uint32_t DEVICE_ERROR_BATTERY_LOW = 2;
const char *temp_mode_name(uint32_t mode);

/// Setpoint limits the app enforces for the Wivar per temperature mode, tenths of °C. The app offers no Asleep
/// setpoint for the Wivar; it gets the union of the others.
static const int32_t SETPOINT_MIN[NUM_SETPOINTS] = {150, 100, 100};
static const int32_t SETPOINT_MAX[NUM_SETPOINTS] = {280, 280, 250};

/// Lowercase hex without separators, for frame dumps in the log.
std::string to_hex(const std::vector<uint8_t> &data);

}  // namespace esphome::zehnder_wivar::proto
