// Host tests for the Wivar protocol layer (no ESPHome, no hardware):
//   tests/run.sh
// Frames are real captures from a Wivar II remote (Steuergerät Modell 1); expected requests match
// the official app's requests byte for byte.
#include "wivar_proto.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using namespace esphome::zehnder_wivar::proto;

static int failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      failures++;                                                      \
    }                                                                  \
  } while (0)

static std::vector<uint8_t> unhex(const char *s) {
  std::vector<uint8_t> v;
  for (; s[0] && s[1]; s += 2) {
    unsigned b;
    std::sscanf(s, "%2x", &b);
    v.push_back(b);
  }
  return v;
}

static std::string hex(const std::vector<uint8_t> &v) {
  std::string out;
  char buf[3];
  for (auto b : v) {
    std::snprintf(buf, sizeof buf, "%02x", b);
    out += buf;
  }
  return out;
}

static const uint8_t TEST_UUID[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                      0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};

// GetRoomsConfirm body as sent by the remote (room "Bad", 22.4 °C, setpoints 20.0/20.0/18.0, window flag set);
// the device's ID string is replaced by zeros.
static const char *ROOM_BODY =
    "0a5f08001203426164180420c80128e00142330800120c3030203030303030303030301a074e6f74205365742000280b3000387342050801"
    "10e0017801800101e00332e803004a05080010c8014a05080210b4014a05080110c801500058016801";

static void test_requests() {
  CHECK(hex(build_request(OP_GET_ROOMS_REQUEST, 1)) == "00000007000508c2012001");
  CHECK(hex(build_set_address(1, TEST_UUID)) == "000000180004080120010a1000112233445566778899aabbccddeeff");
  int32_t sp[NUM_SETPOINTS] = {200, 200, 180};
  CHECK(hex(build_set_setpoints(2, 0, sp)) ==
        "00000020000508cd0120020a1708003205080010c8013205080110c8013205080210b401");
  CHECK(hex(build_preheat_timer(3, 0, 30)) == "0000000f000508ce022003080110011800201e");
  CHECK(hex(build_preheat_timer(4, 0, 0)) == "0000000d000508ce022004080110001800");
  // SetRunState { runMode=Manual(2), userLocation=Away(2) }
  CHECK(hex(build_set_run_state(5, RUN_MODE_MANUAL, LOCATION_AWAY)) == "0000000b000508f301200508021002");
  // GetDayPlan { id=3 }
  CHECK(hex(build_get_plan(OP_GET_DAY_PLAN_REQUEST, 6, 3)) == "00000009000508a10120060803");
}

static void test_frame() {
  auto f = unhex("0000006a000708c301100020010a5f");
  CHECK(frame_size(f) == 0x6a + 4);
  std::vector<uint8_t> garbage = {0xff, 0xff, 0xff, 0xfc, 0x00};
  CHECK(frame_size(garbage) > 4096);  // must not wrap to a small size
  std::vector<uint8_t> shortbuf = {0x00, 0x00};
  CHECK(frame_size(shortbuf) == 0);

  auto confirm = unhex("00000006000408332001");  // SetAddressConfirm, ref 1, empty body (real reply)
  Frame fr;
  CHECK(parse_frame(confirm.data(), confirm.size(), fr));
  CHECK(fr.op_type == OP_SET_ADDRESS_CONFIRM && fr.result == RESULT_OK);
}

static void test_rooms() {
  auto body = unhex(ROOM_BODY);
  std::vector<Room> rooms;
  CHECK(parse_rooms(body.data(), body.size(), rooms));
  CHECK(rooms.size() == 1);
  const Room &r = rooms[0];
  CHECK(r.name == "Bad");
  CHECK(std::fabs(r.measured_temperature - 22.4f) < 0.01f);
  CHECK(std::fabs(r.target_temperature - 20.0f) < 0.01f);
  CHECK(!r.has_humidity && !r.has_valve);
  CHECK(r.setpoints[0] == 200 && r.setpoints[1] == 200 && r.setpoints[2] == 180);
  CHECK(r.open_window_detected && r.open_window_detection_enabled);
  CHECK(r.has_device && r.device_id == 0 && r.signal_strength == 115);
  CHECK(r.device_errors.empty());
  CHECK(r.booster_end == 0);
  CHECK(r.has_fluid_temperature && r.fluid_temperature == 50);  // "Surface temperature" 50 °C
  CHECK(!r.has_overheat_alarm);  // the Wivar doesn't send it
  // Same record with overheatAlarm (cDevice field 30) = true inserted after signalStrength.
  auto hot = body;
  const std::vector<uint8_t> sig = {0x38, 0x73};  // field 7 = 115
  auto at = std::search(hot.begin(), hot.end(), sig.begin(), sig.end());
  CHECK(at != hot.end());
  if (at != hot.end()) {
    const std::vector<uint8_t> alarm = {0xf0, 0x01, 0x01};  // tag (30 << 3) as varint, value 1
    hot.insert(at + 2, alarm.begin(), alarm.end());
    hot[1] += 3;   // room length
    hot[18] += 3;  // device length (cDevice: tag 0x42 at offset 17, length at 18)
    std::vector<Room> hot_rooms;
    CHECK(parse_rooms(hot.data(), hot.size(), hot_rooms) && hot_rooms.size() == 1);
    CHECK(!hot_rooms.empty() && hot_rooms[0].has_overheat_alarm && hot_rooms[0].overheat_alarm);
    CHECK(!hot_rooms.empty() && hot_rooms[0].setpoints[2] == 180);  // rest still parses
  }
}

static void test_negative_temperature() {
  // measuredTemperature -1.2 °C (int32 as 10-byte varint), target 21.5 °C
  auto body = unhex("0a1508001203426164200d28f4ffffffffffffffff0138");
  body.push_back(0x3a);  // humidity 58
  body[1] = body.size() - 2;
  std::vector<Room> rooms;
  CHECK(parse_rooms(body.data(), body.size(), rooms));
  CHECK(rooms.size() == 1 && std::fabs(rooms[0].measured_temperature + 1.2f) < 0.01f);
  CHECK(rooms[0].has_humidity && rooms[0].humidity == 58);
}

static void test_errors() {
  // device { id 0, error 2 (unpacked), error [3,4] (packed) } in room 0
  std::vector<uint8_t> dev = {0x08, 0x00, 0x50, 0x02, 0x52, 0x02, 0x03, 0x04};
  std::vector<uint8_t> room = {0x08, 0x00, 0x42, (uint8_t) dev.size()};
  room.insert(room.end(), dev.begin(), dev.end());
  std::vector<uint8_t> body = {0x0a, (uint8_t) room.size()};
  body.insert(body.end(), room.begin(), room.end());
  std::vector<Room> rooms;
  CHECK(parse_rooms(body.data(), body.size(), rooms));
  CHECK(rooms[0].device_errors == (std::vector<uint32_t>{DEVICE_ERROR_BATTERY_LOW, 3, 4}));
  CHECK(std::string(device_error_name(DEVICE_ERROR_BATTERY_LOW)) == "Battery low");
}

static void test_malformed_lengths() {
  std::vector<Field> fields;
  // Length 2^64 - 11 at pos 11: pos + l wraps to 0, which once made the decoder loop and allocate forever.
  auto wrap = unhex("0af5ffffffffffffffff01");
  CHECK(!decode_message(wrap.data(), wrap.size(), fields));
  // Lengths that wrap a 32-bit size_t (ESP32): 2^32 - 5 and 2^32 + 1.
  auto wrap32 = unhex("0afbffffff0f00");
  CHECK(!decode_message(wrap32.data(), wrap32.size(), fields));
  auto big32 = unhex("0a818080801000");
  CHECK(!decode_message(big32.data(), big32.size(), fields));
  // One byte longer than the rest of the message.
  auto over = unhex("0a03aabb");
  CHECK(!decode_message(over.data(), over.size(), fields));
  // Exactly fits.
  auto exact = unhex("0a02aabb");
  fields.clear();
  CHECK(decode_message(exact.data(), exact.size(), fields) && fields.size() == 1 && fields[0].len == 2);
  // Varints longer than 64 bits: a 10th byte above 1, or an 11th byte.
  auto ovf = unhex("08ffffffffffffffffff02");
  CHECK(!decode_message(ovf.data(), ovf.size(), fields));
  auto max = unhex("08ffffffffffffffffff01");
  fields.clear();
  CHECK(decode_message(max.data(), max.size(), fields) && fields[0].value == UINT64_MAX);
  auto eleven = unhex("08ffffffffffffffffff8100");
  CHECK(!decode_message(eleven.data(), eleven.size(), fields));
  // Truncated fixed64 and fixed32.
  auto f64 = unhex("0901020304050607");
  CHECK(!decode_message(f64.data(), f64.size(), fields));
  auto f32 = unhex("0d010203");
  CHECK(!decode_message(f32.data(), f32.size(), fields));
  // Frame length 2^32 - 4: total + 4 wraps to 0 in 32 bits, and the op length would then read past the buffer.
  auto frame = unhex("fffffffc00ff0800");
  Frame fr;
  CHECK(!parse_frame(frame.data(), frame.size(), fr));
}

static void test_run_state() {
  auto rs_body = unhex("0a0c080110001804280030004000");  // real: Automatic / Home Awake
  RunState rs;
  CHECK(parse_run_state(rs_body.data(), rs_body.size(), rs));
  CHECK(rs.run_mode == 1 && rs.temp_mode == 0 && !rs.standby && rs.device_errors.empty());
  CHECK(std::string(run_mode_name(rs.run_mode)) == "Automatic");

  std::vector<uint8_t> inner = {0x08, 0x01, 0x5a, 0x01, 0x02};  // runMode 1, deviceErrors [BatteryLow] packed
  std::vector<uint8_t> body = {0x0a, (uint8_t) inner.size()};
  body.insert(body.end(), inner.begin(), inner.end());
  RunState rs2;
  CHECK(parse_run_state(body.data(), body.size(), rs2));
  CHECK(rs2.device_errors == std::vector<uint32_t>{DEVICE_ERROR_BATTERY_LOW});
}

static void test_version() {
  auto body = unhex("0891884012074e6f74205365741800");  // real VersionConfirm: systemVersion 1049617, "Not Set"
  uint32_t v = 0;
  CHECK(parse_version(body.data(), body.size(), v));
  CHECK(v == 1049617);
}

static void test_schedule() {
  // GetScheduleIdsConfirm: day plan ids packed [1, 2], week plan id 7 unpacked.
  std::vector<uint8_t> ids = {0x0a, 0x02, 0x01, 0x02, 0x10, 0x07};
  std::vector<uint32_t> days, weeks;
  CHECK(parse_schedule_ids(ids.data(), ids.size(), days, weeks));
  CHECK((days == std::vector<uint32_t>{1, 2}) && (weeks == std::vector<uint32_t>{7}));

  // GetDayPlanConfirm { dPlan { id 1, name "W", items 22:00 Away, 07:00 Home (unsorted), preheat 06:30 90 min } }
  auto varint = [](std::vector<uint8_t> &out, uint64_t v) {
    while (v > 0x7F) {
      out.push_back((v & 0x7F) | 0x80);
      v >>= 7;
    }
    out.push_back(v);
  };
  auto fvar = [&](std::vector<uint8_t> &out, uint32_t field, uint64_t v) {
    varint(out, field << 3);
    varint(out, v);
  };
  auto fmsg = [&](std::vector<uint8_t> &out, uint32_t field, const std::vector<uint8_t> &m) {
    varint(out, (field << 3) | 2);
    varint(out, m.size());
    out.insert(out.end(), m.begin(), m.end());
  };
  std::vector<uint8_t> away, home, preheat, plan, body;
  fvar(away, 1, 22 * 60);  // minutes
  fvar(away, 2, 2);
  fvar(home, 1, 7 * 60);
  fvar(home, 2, 0);
  fvar(preheat, 1, 6 * 60 + 30);
  fvar(preheat, 2, 0);
  fvar(preheat, 3, 90);
  fvar(plan, 1, 1);
  fmsg(plan, 2, {'W'});
  fmsg(plan, 3, away);
  fmsg(plan, 3, home);
  fmsg(plan, 20, preheat);
  fmsg(body, 1, plan);
  DayPlan dp;
  CHECK(parse_day_plan(body.data(), body.size(), dp));
  CHECK(dp.id == 1 && dp.name == "W" && dp.items.size() == 2 && dp.functions.size() == 1);
  CHECK(describe_day_plan(dp) == "07:00 Home, 22:00 Away, Preheat 06:30 90 min");

  // GetWeekPlanConfirm { wPlan { id 7, monday 1 .. friday 1, saturday 2, sunday 2 } }
  std::vector<uint8_t> wp = {0x08, 0x07, 0x18, 1, 0x20, 1, 0x28, 1, 0x30, 1, 0x38, 1, 0x40, 2, 0x48, 2};
  std::vector<uint8_t> wbody = {0x0a, (uint8_t) wp.size()};
  wbody.insert(wbody.end(), wp.begin(), wp.end());
  WeekPlan w;
  CHECK(parse_week_plan(wbody.data(), wbody.size(), w));
  CHECK(w.id == 7 && w.has_day[0] && w.day_ids[0] == 1 && w.day_ids[4] == 1 && w.day_ids[5] == 2 && w.day_ids[6] == 2);

  DayPlan empty;
  CHECK(describe_day_plan(empty) == "-");
}

// EU summer time: from the last Sunday of March to the last Sunday of October, 01:00 UTC.
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned) (y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t) doe - 719468;
}
static int64_t last_sunday_0100(int64_t year, unsigned month) {
  int64_t last = days_from_civil(year, month, 31);
  int64_t weekday = (last + 4) % 7;  // 1970-01-01 was a Thursday; 0 = Sunday
  return (last - weekday) * 86400 + 3600;
}
static bool eu_dst(int64_t t, const void *) {
  int64_t year = 1970 + t / 31556952;
  for (int64_t y = year - 1; y <= year + 1; y++)
    if (t >= last_sunday_0100(y, 3) && t < last_sunday_0100(y, 10))
      return true;
  return false;
}

static void test_dst_switches() {
  // 2026-10-14 05:00 UTC, Central European Time (CET/CEST). Expected local switch times from Python zoneinfo:
  // 2026-10-25 02:00 winter, 2027-03-28 03:00 summer, 2027-10-31 02:00 winter, 2028-03-26 03:00 summer.
  auto sw = find_dst_switches(1791954000, eu_dst, nullptr, -3600, -7200);
  CHECK(sw.size() == 4);
  if (sw.size() == 4) {
    CHECK(sw[0].local_time == 1792893600 && !sw[0].summer_after);
    CHECK(sw[1].local_time == 1806202800 && sw[1].summer_after);
    CHECK(sw[2].local_time == 1824948000 && !sw[2].summer_after);
    CHECK(sw[3].local_time == 1837652400 && sw[3].summer_after);
  }
}

static void test_plan_writes() {
  DayPlan dp;
  dp.id = 3;
  dp.name = "W";
  dp.items = {{0, 2}, {6 * 3600 + 20 * 60, 0}, {6 * 3600 + 45 * 60, 2}};
  FunctionPlanItem pre;
  pre.start_s = 6 * 3600;
  pre.duration_min = 30;
  pre.has_device = true;
  pre.device_id = 0;
  dp.functions = {pre};
  CHECK(validate_day_plan(dp).empty());
  // SetDayplanRequest { dPlan(1) } has the GetDayPlanConfirm layout: encode, parse back, compare.
  auto frame = build_set_day_plan(4, dp);
  Frame f;
  CHECK(parse_frame(frame.data(), frame.size(), f) && f.op_type == OP_SET_DAY_PLAN_REQUEST);
  DayPlan back;
  CHECK(parse_day_plan(f.body, f.body_len, back));
  CHECK(same_plan(dp, back) && back.name == "W" && back.functions[0].has_device && !back.functions[0].has_room);
  // Minutes on the wire: 06:20 = 380.
  CHECK(hex(frame).find("08fc021000") != std::string::npos);

  auto bad = dp;
  bad.items[0].start_s = 60;
  CHECK(validate_day_plan(bad) == "the first entry must start at 00:00");
  bad = dp;
  bad.items[1].start_s = 6 * 3600 + 21 * 60;
  CHECK(!validate_day_plan(bad).empty());  // not a 5-minute step
  bad = dp;
  bad.items[2].start_s = 6 * 3600 + 25 * 60;
  CHECK(!validate_day_plan(bad).empty());  // 5 minutes after the previous entry
  bad = dp;
  bad.items[1].temp_mode = 1;
  CHECK(validate_day_plan(bad) == "entries must be Home or Away");
  bad = dp;
  bad.functions[0].duration_min = 20;
  CHECK(!validate_day_plan(bad).empty());
  bad = dp;
  bad.items.clear();
  CHECK(!validate_day_plan(bad).empty());
  bad = dp;
  for (int i = 3; i < 14; i++)
    bad.items.push_back({(uint32_t) (8 + i) * 3600, (uint32_t) (i % 2 ? 0 : 2)});
  CHECK(validate_day_plan(bad) == "a day plan needs 1-13 entries");

  WeekPlan wp;
  wp.id = 0;
  for (int d = 0; d < 7; d++) {
    wp.has_day[d] = true;
    wp.day_ids[d] = d < 5 ? 0 : 1;
  }
  CHECK(validate_week_plan(wp).empty());
  auto wframe = build_set_week_plan(5, wp);
  CHECK(parse_frame(wframe.data(), wframe.size(), f) && f.op_type == OP_SET_WEEK_PLAN_REQUEST);
  WeekPlan wback;
  CHECK(parse_week_plan(f.body, f.body_len, wback) && same_plan(wp, wback));
  wp.has_day[6] = false;
  CHECK(!validate_week_plan(wp).empty());
}

static void test_time() {
  // SetTime { utc 1800000000, summer, switch at local 1811000000 to winter }
  std::vector<DstSwitch> sw = {{1811000000, false}};
  CHECK(hex(build_set_time(9, 1800000000, true, sw)) ==
        "00000019000508860220090880a4a7da0610031a0808c0d5c6df061002");
  // GetTimeConfirm { utc 100, local 7300, daylightSaving 3, switch { 200, 2 } }
  std::vector<uint8_t> body = {0x08, 0x64, 0x10, 0xd4, 0x38, 0x18, 0x03, 0x22, 0x05, 0x08, 0xc8, 0x01, 0x10, 0x02};
  RemoteTime t;
  CHECK(parse_time(body.data(), body.size(), t));
  CHECK(t.utc == 100 && t.local == 7252 && t.daylight_saving == 3);
  CHECK(t.switches.size() == 1 && t.switches[0].local_time == 200 && !t.switches[0].summer_after);
}

int main() {
  test_requests();
  test_frame();
  test_rooms();
  test_negative_temperature();
  test_errors();
  test_malformed_lengths();
  test_run_state();
  test_version();
  test_schedule();
  test_time();
  test_dst_switches();
  test_plan_writes();
  if (failures == 0)
    std::printf("all protocol tests passed\n");
  return failures == 0 ? 0 : 1;
}
