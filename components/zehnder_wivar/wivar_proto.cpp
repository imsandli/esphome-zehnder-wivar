#include "wivar_proto.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace esphome::zehnder_wivar::proto {

static void put_varint(std::vector<uint8_t> &out, uint64_t v) {
  while (v > 0x7F) {
    out.push_back(static_cast<uint8_t>((v & 0x7F) | 0x80));
    v >>= 7;
  }
  out.push_back(static_cast<uint8_t>(v));
}

static void put_field_varint(std::vector<uint8_t> &out, uint32_t field, uint64_t v) {
  put_varint(out, (field << 3) | 0);
  put_varint(out, v);
}

static void put_field_bytes(std::vector<uint8_t> &out, uint32_t field, const uint8_t *data, size_t len) {
  put_varint(out, (field << 3) | 2);
  put_varint(out, len);
  out.insert(out.end(), data, data + len);
}

static bool get_varint(const uint8_t *data, size_t len, size_t &pos, uint64_t &out) {
  out = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    if (pos >= len)
      return false;
    uint8_t b = data[pos++];
    if (shift == 63 && (b & 0x7E))  // the 10th byte holds only bit 63
      return false;
    out |= static_cast<uint64_t>(b & 0x7F) << shift;
    if (!(b & 0x80))
      return true;
  }
  return false;
}

bool decode_message(const uint8_t *data, size_t len, std::vector<Field> &out) {
  size_t pos = 0;
  while (pos < len) {
    uint64_t tag;
    if (!get_varint(data, len, pos, tag))
      return false;
    Field f{static_cast<uint32_t>(tag >> 3), static_cast<uint8_t>(tag & 7), 0, nullptr, 0};
    switch (f.wire_type) {
      case 0:
        if (!get_varint(data, len, pos, f.value))
          return false;
        break;
      case 1:  // fixed64
        if (len - pos < 8)
          return false;
        f.data = data + pos;
        f.len = 8;
        pos += 8;
        break;
      case 2: {
        uint64_t l;
        // l is untrusted and up to 2^64 - 1: pos + l could wrap around
        if (!get_varint(data, len, pos, l) || l > len - pos)
          return false;
        f.data = data + pos;
        f.len = l;
        pos += l;
        break;
      }
      case 5:  // fixed32
        if (len - pos < 4)
          return false;
        f.data = data + pos;
        f.len = 4;
        pos += 4;
        break;
      default:
        return false;
    }
    out.push_back(f);
  }
  return true;
}

static std::vector<uint8_t> wrap_frame(const std::vector<uint8_t> &op, const std::vector<uint8_t> &body) {
  uint32_t total = op.size() + 2 + body.size();
  std::vector<uint8_t> frame = {
      static_cast<uint8_t>(total >> 24), static_cast<uint8_t>(total >> 16), static_cast<uint8_t>(total >> 8),
      static_cast<uint8_t>(total),       static_cast<uint8_t>(op.size() >> 8), static_cast<uint8_t>(op.size()),
  };
  frame.insert(frame.end(), op.begin(), op.end());
  frame.insert(frame.end(), body.begin(), body.end());
  return frame;
}

static std::vector<uint8_t> gateway_op(uint32_t op_type, uint32_t reference) {
  std::vector<uint8_t> op;
  put_field_varint(op, 1, op_type);
  put_field_varint(op, 4, reference);
  return op;
}

std::vector<uint8_t> build_request(uint32_t op_type, uint32_t reference) {
  return wrap_frame(gateway_op(op_type, reference), {});
}

std::vector<uint8_t> build_pairing_request(uint32_t reference, const uint8_t app_uuid[16], const uint8_t nonce[16],
                                           const std::string &nickname) {
  std::vector<uint8_t> app_pairing;
  put_field_bytes(app_pairing, 1, app_uuid, 16);
  put_field_bytes(app_pairing, 2, reinterpret_cast<const uint8_t *>(nickname.data()), nickname.size());
  std::vector<uint8_t> body;
  put_field_bytes(body, 1, app_pairing.data(), app_pairing.size());
  put_field_bytes(body, 2, nonce, 16);
  return wrap_frame(gateway_op(OP_APP_PAIRING_REQUEST, reference), body);
}

std::vector<uint8_t> build_set_address(uint32_t reference, const uint8_t app_uuid[16]) {
  std::vector<uint8_t> body;
  put_field_bytes(body, 1, app_uuid, 16);
  return wrap_frame(gateway_op(OP_SET_ADDRESS_REQUEST, reference), body);
}

std::vector<uint8_t> build_set_setpoints(uint32_t reference, uint32_t room_id, const int32_t setpoints[NUM_SETPOINTS]) {
  // SetRoomValueRequest { roomValue(1): cRoomValue { id(1), repeated temperatures(6): cRoomTemperature {mode, value} } }
  std::vector<uint8_t> room_value;
  put_field_varint(room_value, 1, room_id);
  for (int mode = 0; mode < NUM_SETPOINTS; mode++) {
    if (setpoints[mode] < 0)
      continue;
    std::vector<uint8_t> temp;
    put_field_varint(temp, 1, mode);
    put_field_varint(temp, 2, setpoints[mode]);
    put_field_bytes(room_value, 6, temp.data(), temp.size());
  }
  std::vector<uint8_t> body;
  put_field_bytes(body, 1, room_value.data(), room_value.size());
  return wrap_frame(gateway_op(OP_SET_ROOM_VALUE_REQUEST, reference), body);
}

std::vector<uint8_t> build_preheat_timer(uint32_t reference, uint32_t device_id, uint32_t minutes) {
  // SetDeviceFunctionModeRequest { mode(1)=TOWEL_PREHEAT, state(2)=ON/OFF, deviceId(3), duration(4) minutes }
  std::vector<uint8_t> body;
  put_field_varint(body, 1, 1);
  put_field_varint(body, 2, minutes > 0 ? 1 : 0);
  put_field_varint(body, 3, device_id);
  if (minutes > 0)
    put_field_varint(body, 4, minutes);
  return wrap_frame(gateway_op(OP_SET_DEVICE_FUNCTION_MODE_REQUEST, reference), body);
}

std::vector<uint8_t> build_set_run_state(uint32_t reference, uint32_t run_mode, uint32_t location) {
  std::vector<uint8_t> body;
  put_field_varint(body, 1, run_mode);
  put_field_varint(body, 2, location);
  return wrap_frame(gateway_op(OP_SET_RUN_STATE_REQUEST, reference), body);
}

std::vector<DstSwitch> find_dst_switches(int64_t utc, bool (*in_dst)(int64_t t, const void *ctx), const void *ctx,
                                         int32_t std_offset, int32_t dst_offset, size_t max) {
  // Day steps, then narrow each change down to the second.
  std::vector<DstSwitch> out;
  const int64_t day = 86400;
  int64_t t = utc;
  bool state = in_dst(t, ctx);
  for (int i = 0; i < 2 * 366 && out.size() < max; i++, t += day) {
    bool next = in_dst(t + day, ctx);
    if (next == state)
      continue;
    int64_t lo = t, hi = t + day;  // in_dst(lo) == state, in_dst(hi) == next
    while (hi - lo > 1) {
      int64_t mid = lo + (hi - lo) / 2;
      (in_dst(mid, ctx) == state ? lo : hi) = mid;
    }
    out.push_back({(uint64_t) (hi - (next ? dst_offset : std_offset)), next});
    state = next;
  }
  return out;
}

std::vector<uint8_t> build_set_time(uint32_t reference, uint64_t utc, bool summer, const std::vector<DstSwitch> &switches) {
  std::vector<uint8_t> body;
  put_field_varint(body, 1, utc);
  put_field_varint(body, 2, summer ? 3 : 2);
  for (auto &sw : switches) {
    std::vector<uint8_t> item;
    put_field_varint(item, 1, sw.local_time);
    put_field_varint(item, 2, sw.summer_after ? 3 : 2);
    put_field_bytes(body, 3, item.data(), item.size());
  }
  return wrap_frame(gateway_op(OP_SET_TIME_REQUEST, reference), body);
}

std::vector<uint8_t> build_get_plan(uint32_t op_type, uint32_t reference, uint32_t id) {
  std::vector<uint8_t> body;
  put_field_varint(body, 1, id);
  return wrap_frame(gateway_op(op_type, reference), body);
}

size_t frame_size(const std::vector<uint8_t> &buf) {
  if (buf.size() < 4)
    return 0;
  uint32_t total = (uint32_t(buf[0]) << 24) | (uint32_t(buf[1]) << 16) | (uint32_t(buf[2]) << 8) | buf[3];
  // Garbage headers must not wrap around to a small size (the caller rejects anything this large).
  if (total > 0x7FFFFFF0u)
    return SIZE_MAX;
  return total + 4;
}

bool parse_frame(const uint8_t *data, size_t len, Frame &out) {
  if (len < 6)
    return false;
  uint32_t total = (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) | (uint32_t(data[2]) << 8) | data[3];
  size_t op_len = (size_t(data[4]) << 8) | data[5];
  if (total > len - 4 || op_len + 2 > total)
    return false;
  std::vector<Field> fields;
  if (!decode_message(data + 6, op_len, fields))
    return false;
  for (auto &f : fields) {
    if (f.wire_type != 0)
      continue;
    if (f.number == 1)
      out.op_type = f.value;
    else if (f.number == 2)
      out.result = f.value;
    else if (f.number == 4)
      out.reference = f.value;
  }
  out.body = data + 6 + op_len;
  out.body_len = total - op_len - 2;
  return true;
}

// Repeated varint field, packed (wire type 2) or not (wire type 0).
static void append_varints(const Field &f, std::vector<uint32_t> &out) {
  if (f.wire_type == 0) {
    out.push_back(f.value);
    return;
  }
  if (f.wire_type != 2)
    return;
  size_t pos = 0;
  uint64_t v;
  while (pos < f.len && get_varint(f.data, f.len, pos, v))
    out.push_back(v);
}

static int32_t as_int32(uint64_t v) { return static_cast<int32_t>(static_cast<uint32_t>(v)); }

// cDevice (room field 8): id (1), signalStrength (7), overheatAlarm (30), preheat end (39) / duration (61),
// fluidTemperature (60), and valve state via cActuator (9) -> value (2), TemperatureActuator type = 1.
static void parse_device(const Field &device, Room &room) {
  std::vector<Field> dev;
  if (!decode_message(device.data, device.len, dev))
    return;
  bool first = !room.has_device;
  room.has_device = true;
  for (auto &d : dev) {
    if (d.number == 10)
      append_varints(d, room.device_errors);
    if (first && d.wire_type == 0) {
      if (d.number == 1)
        room.device_id = d.value;
      else if (d.number == 7) {
        room.has_signal_strength = true;
        room.signal_strength = d.value;
      } else if (d.number == 39)
        room.preheat_end = d.value;
      else if (d.number == 61)
        room.preheat_duration = d.value;
      else if (d.number == 30) {
        room.has_overheat_alarm = true;
        room.overheat_alarm = d.value != 0;
      } else if (d.number == 60) {
        room.has_fluid_temperature = true;
        room.fluid_temperature = d.value;
      }
    }
    if (d.number != 9 || d.wire_type != 2)
      continue;
    std::vector<Field> act;
    if (!decode_message(d.data, d.len, act))
      continue;
    uint64_t type = 0;
    bool has_value = false;
    uint64_t value = 0;
    for (auto &a : act) {
      if (a.number == 1 && a.wire_type == 0)
        type = a.value;
      else if (a.number == 2 && a.wire_type == 0) {
        value = a.value;
        has_value = true;
      }
    }
    if (type == 1 && has_value) {
      room.has_valve = true;
      room.valve = as_int32(value);
    }
  }
}

bool parse_rooms(const uint8_t *body, size_t len, std::vector<Room> &rooms) {
  std::vector<Field> msg;
  if (!decode_message(body, len, msg))
    return false;
  for (auto &rf : msg) {
    if (rf.number != 1 || rf.wire_type != 2)
      continue;
    std::vector<Field> r;
    if (!decode_message(rf.data, rf.len, r))
      return false;
    Room room;
    for (auto &f : r) {
      switch (f.number) {
        case 1:
          room.id = f.value;
          break;
        case 2:
          if (f.wire_type == 2)
            room.name.assign(reinterpret_cast<const char *>(f.data), f.len);
          break;
        case 4:
          room.has_target_temperature = true;
          room.target_temperature = f.value / 10.0f;
          break;
        case 5:
          room.has_measured_temperature = true;
          room.measured_temperature = as_int32(f.value) / 10.0f;
          break;
        case 7:
          room.has_humidity = true;
          room.humidity = f.value;
          break;
        case 8:
          if (f.wire_type == 2)
            parse_device(f, room);
          break;
        case 9: {  // cRoomTemperature { mode(1), value(2) }
          std::vector<Field> t;
          if (f.wire_type != 2 || !decode_message(f.data, f.len, t))
            break;
          uint64_t mode = 0, value = 0;
          for (auto &g : t) {
            if (g.number == 1 && g.wire_type == 0)
              mode = g.value;
            else if (g.number == 2 && g.wire_type == 0)
              value = g.value;
          }
          if (mode < NUM_SETPOINTS)
            room.setpoints[mode] = value;
          break;
        }
        case 11:
          room.open_window_detection_enabled = f.value != 0;
          break;
        case 13:
          room.open_window_detected = f.value != 0;
          break;
        case 16:
          room.booster_end = f.value;
          break;
        default:
          break;
      }
    }
    rooms.push_back(room);
  }
  return true;
}

bool parse_run_state(const uint8_t *body, size_t len, RunState &out) {
  std::vector<Field> msg;
  if (!decode_message(body, len, msg))
    return false;
  for (auto &f : msg) {
    if (f.number != 1 || f.wire_type != 2)
      continue;
    std::vector<Field> rs;
    if (!decode_message(f.data, f.len, rs))
      return false;
    for (auto &g : rs) {
      if (g.number == 11) {
        append_varints(g, out.device_errors);
        continue;
      }
      if (g.wire_type != 0)
        continue;
      if (g.number == 1)
        out.run_mode = g.value;
      else if (g.number == 2)
        out.temp_mode = g.value;
      else if (g.number == 3)
        out.following_schedule = g.value;
      else if (g.number == 7)
        out.standby = g.value != 0;
      else if (g.number == 5) {
        out.has_today_dayplan = true;
        out.today_dayplan_id = g.value;
      } else if (g.number == 6) {
        out.has_today_weekplan = true;
        out.today_weekplan_id = g.value;
      }
    }
    return true;
  }
  return false;
}

bool parse_time(const uint8_t *body, size_t len, RemoteTime &out) {
  std::vector<Field> msg;
  if (!decode_message(body, len, msg))
    return false;
  bool has_utc = false;
  for (auto &f : msg) {
    if (f.wire_type == 0 && f.number == 1) {
      out.utc = f.value;
      has_utc = true;
    } else if (f.wire_type == 0 && f.number == 2) {
      out.local = f.value;
    } else if (f.wire_type == 0 && f.number == 3) {
      out.daylight_saving = f.value;
    } else if (f.wire_type == 2 && f.number == 4) {
      std::vector<Field> sw;
      if (!decode_message(f.data, f.len, sw))
        return false;
      DstSwitch item;
      for (auto &g : sw) {
        if (g.wire_type == 0 && g.number == 1)
          item.local_time = g.value;
        else if (g.wire_type == 0 && g.number == 2)
          item.summer_after = g.value == 3;
      }
      out.switches.push_back(item);
    }
  }
  return has_utc;
}

bool parse_version(const uint8_t *body, size_t len, uint32_t &version) {
  std::vector<Field> msg;
  if (!decode_message(body, len, msg))
    return false;
  for (auto &f : msg) {
    if (f.number == 1 && f.wire_type == 0) {
      version = f.value;
      return true;
    }
  }
  return false;
}

// ── Schedule ─────────────────────────────────────────────────────────────────

// The body's first length-delimited field `number`, decoded.
static bool sub_message(const uint8_t *body, size_t len, uint32_t number, std::vector<Field> &out) {
  std::vector<Field> msg;
  if (!decode_message(body, len, msg))
    return false;
  for (auto &f : msg)
    if (f.number == number && f.wire_type == 2)
      return decode_message(f.data, f.len, out);
  return false;
}

bool parse_schedule_ids(const uint8_t *body, size_t len, std::vector<uint32_t> &day_ids, std::vector<uint32_t> &week_ids) {
  std::vector<Field> msg;
  if (!decode_message(body, len, msg))
    return false;
  for (auto &f : msg) {
    if (f.number == 1)
      append_varints(f, day_ids);
    else if (f.number == 2)
      append_varints(f, week_ids);
  }
  return true;
}

bool parse_day_plan(const uint8_t *body, size_t len, DayPlan &out) {
  std::vector<Field> plan;
  if (!sub_message(body, len, 1, plan))
    return false;
  for (auto &f : plan) {
    if (f.number == 1 && f.wire_type == 0) {
      out.id = f.value;
    } else if (f.number == 2 && f.wire_type == 2) {
      out.name.assign(reinterpret_cast<const char *>(f.data), f.len);
    } else if ((f.number == 3 || f.number == 20) && f.wire_type == 2) {
      std::vector<Field> item;
      if (!decode_message(f.data, f.len, item))
        return false;
      uint64_t v[6]{};
      bool has[6]{};
      for (auto &g : item)
        if (g.wire_type == 0 && g.number < 6) {
          v[g.number] = g.value;
          has[g.number] = true;
        }
      // startTime is in minutes after midnight on the wire (seconds in the decoded plan).
      if (f.number == 3) {
        out.items.push_back({(uint32_t) v[1] * 60, (uint32_t) v[2]});
      } else {
        FunctionPlanItem fn;
        fn.start_s = (uint32_t) v[1] * 60;
        fn.mode = (uint32_t) v[2];
        fn.duration_min = (uint32_t) v[3];
        fn.has_room = has[4];
        fn.room_id = (uint32_t) v[4];
        fn.has_device = has[5];
        fn.device_id = (uint32_t) v[5];
        out.functions.push_back(fn);
      }
    }
  }
  std::sort(out.items.begin(), out.items.end(), [](auto &a, auto &b) { return a.start_s < b.start_s; });
  std::sort(out.functions.begin(), out.functions.end(), [](auto &a, auto &b) { return a.start_s < b.start_s; });
  return true;
}

bool parse_week_plan(const uint8_t *body, size_t len, WeekPlan &out) {
  std::vector<Field> plan;
  if (!sub_message(body, len, 1, plan))
    return false;
  for (auto &f : plan) {
    if (f.number == 1 && f.wire_type == 0) {
      out.id = f.value;
    } else if (f.number == 2 && f.wire_type == 2) {
      out.name.assign(reinterpret_cast<const char *>(f.data), f.len);
    } else if (f.number >= 3 && f.number <= 9 && f.wire_type == 0) {
      out.has_day[f.number - 3] = true;
      out.day_ids[f.number - 3] = f.value;
    }
  }
  return true;
}

static std::string hhmm(uint32_t seconds) {
  char buf[8];
  snprintf(buf, sizeof buf, "%02u:%02u", (unsigned) (seconds / 3600 % 24), (unsigned) (seconds / 60 % 60));
  return buf;
}

const char *short_temp_mode_name(uint32_t mode) {
  switch (mode) {
    case 0:
      return "Home";
    case 1:
      return "Asleep";
    case 2:
      return "Away";
    case 3:
      return "Antifreeze";
    default:
      return "?";
  }
}

std::string describe_day_plan(const DayPlan &plan) {
  std::string out;
  for (auto &i : plan.items) {
    if (!out.empty())
      out += ", ";
    out += hhmm(i.start_s) + " " + short_temp_mode_name(i.temp_mode);
  }
  for (auto &f : plan.functions) {
    if (!out.empty())
      out += ", ";
    out += std::string(f.mode == 0 ? "Preheat " : "Function ") + hhmm(f.start_s) + " " + std::to_string(f.duration_min) +
           " min";
  }
  return out.empty() ? "-" : out;
}

bool same_plan(const DayPlan &a, const DayPlan &b) {
  if (a.id != b.id || a.items.size() != b.items.size() || a.functions.size() != b.functions.size())
    return false;
  for (size_t i = 0; i < a.items.size(); i++)
    if (a.items[i].start_s != b.items[i].start_s || a.items[i].temp_mode != b.items[i].temp_mode)
      return false;
  for (size_t i = 0; i < a.functions.size(); i++) {
    auto &x = a.functions[i];
    auto &y = b.functions[i];
    if (x.start_s != y.start_s || x.mode != y.mode || x.duration_min != y.duration_min)
      return false;
  }
  return true;
}

bool same_plan(const WeekPlan &a, const WeekPlan &b) {
  if (a.id != b.id)
    return false;
  for (int d = 0; d < 7; d++)
    if (a.has_day[d] != b.has_day[d] || a.day_ids[d] != b.day_ids[d])
      return false;
  return true;
}

static std::vector<uint8_t> day_plan_body(const DayPlan &plan);
static std::vector<uint8_t> week_plan_body(const WeekPlan &plan);

std::vector<uint8_t> build_set_day_plan(uint32_t reference, const DayPlan &plan) {
  return wrap_frame(gateway_op(OP_SET_DAY_PLAN_REQUEST, reference), day_plan_body(plan));
}

std::vector<uint8_t> build_add_day_plan(uint32_t reference, const DayPlan &plan) {
  return wrap_frame(gateway_op(OP_ADD_DAY_PLAN_REQUEST, reference), day_plan_body(plan));
}

std::vector<uint8_t> build_set_week_plan(uint32_t reference, const WeekPlan &plan) {
  return wrap_frame(gateway_op(OP_SET_WEEK_PLAN_REQUEST, reference), week_plan_body(plan));
}

std::vector<uint8_t> build_add_week_plan(uint32_t reference, const WeekPlan &plan) {
  return wrap_frame(gateway_op(OP_ADD_WEEK_PLAN_REQUEST, reference), week_plan_body(plan));
}

std::vector<uint8_t> build_remove_plan(uint32_t op_type, uint32_t reference, uint32_t id) {
  return build_get_plan(op_type, reference, id);  // same body: { id(1) }
}

static std::vector<uint8_t> day_plan_body(const DayPlan &plan) {
  std::vector<uint8_t> dp;
  put_field_varint(dp, 1, plan.id);
  put_field_bytes(dp, 2, reinterpret_cast<const uint8_t *>(plan.name.data()), plan.name.size());
  for (auto &item : plan.items) {
    std::vector<uint8_t> it;
    put_field_varint(it, 1, item.start_s / 60);
    put_field_varint(it, 2, item.temp_mode);
    put_field_bytes(dp, 3, it.data(), it.size());
  }
  for (auto &fn : plan.functions) {
    std::vector<uint8_t> it;
    put_field_varint(it, 1, fn.start_s / 60);
    put_field_varint(it, 2, fn.mode);
    put_field_varint(it, 3, fn.duration_min);
    if (fn.has_room)
      put_field_varint(it, 4, fn.room_id);
    if (fn.has_device)
      put_field_varint(it, 5, fn.device_id);
    put_field_bytes(dp, 20, it.data(), it.size());
  }
  std::vector<uint8_t> body;
  put_field_bytes(body, 1, dp.data(), dp.size());
  return body;
}

static std::vector<uint8_t> week_plan_body(const WeekPlan &plan) {
  std::vector<uint8_t> wp;
  put_field_varint(wp, 1, plan.id);
  put_field_bytes(wp, 2, reinterpret_cast<const uint8_t *>(plan.name.data()), plan.name.size());
  for (int d = 0; d < 7; d++)
    if (plan.has_day[d])
      put_field_varint(wp, 3 + d, plan.day_ids[d]);
  std::vector<uint8_t> body;
  put_field_bytes(body, 1, wp.data(), wp.size());
  return body;
}

std::string validate_day_plan(const DayPlan &plan) {
  if (plan.items.empty() || plan.items.size() > MAX_DAY_PLAN_ITEMS)
    return "a day plan needs 1-13 entries";
  if (plan.items[0].start_s != 0)
    return "the first entry must start at 00:00";
  for (size_t i = 0; i < plan.items.size(); i++) {
    auto &item = plan.items[i];
    if (item.start_s >= 24 * 3600 || item.start_s % 300 != 0)
      return "entry times must be 5-minute steps within the day";
    if (item.temp_mode != 0 && item.temp_mode != 2)
      return "entries must be Home or Away";
    if (i > 0 && item.start_s < plan.items[i - 1].start_s + 600)
      return "entries must be in order and at least 10 minutes apart";
  }
  for (auto &fn : plan.functions) {
    if (fn.mode != 0)
      return "only the towel preheat timer is supported";
    if (fn.start_s >= 24 * 3600 || fn.start_s % 300 != 0)
      return "timer start must be a 5-minute step within the day";
    if (fn.duration_min < 15 || fn.duration_min > 240 || fn.duration_min % 15 != 0)
      return "timer duration must be 15-240 minutes in 15-minute steps";
  }
  if (plan.name.size() > 32)
    return "name longer than 32 characters";
  return "";
}

std::string validate_week_plan(const WeekPlan &plan) {
  for (bool has : plan.has_day)
    if (!has)
      return "a week plan needs a day plan for all seven days";
  if (plan.name.size() > 32)
    return "name longer than 32 characters";
  return "";
}

const char *run_mode_name(uint32_t mode) {
  switch (mode) {
    case 1:
      return "Automatic";
    case 2:
      return "Manual";
    case 3:
      return "Antifreeze";
    case 4:
      return "Off";
    default:
      return "Unknown";
  }
}

const char *device_error_name(uint32_t code) {
  switch (code) {
    case 1:
      return "No error";
    case 2:
      return "Battery low";
    case 3:
      return "Signal poor";
    case 4:
      return "Signal lost";
    case 5:
      return "Valve detached";
    case 6:
      return "No warm water";
    case 7:
      return "Room sensor error";
    case 8:
      return "Medium sensor error";
    default:
      return "Unknown error";
  }
}

const char *temp_mode_name(uint32_t mode) {
  switch (mode) {
    case 0:
      return "Home Awake";
    case 1:
      return "Home Asleep";
    case 2:
      return "Away";
    case 3:
      return "Antifreeze";
    case 4:
      return "Override";
    default:
      return "Unknown";
  }
}

std::string to_hex(const std::vector<uint8_t> &data) {
  static const char DIGITS[] = "0123456789abcdef";
  std::string out;
  out.reserve(data.size() * 2);
  for (uint8_t b : data) {
    out.push_back(DIGITS[b >> 4]);
    out.push_back(DIGITS[b & 0x0F]);
  }
  return out;
}

}  // namespace esphome::zehnder_wivar::proto
