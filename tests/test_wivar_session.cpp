// Host tests for the Wivar session logic against a scripted fake remote (no ESPHome, no hardware):
//   tests/run.sh
// The fake remote answers like the real one: replies in 20-byte chunks, the next chunk only after a read
// ("poke"), optionally no first chunk until poked, and rooms updates instead of write confirms.
#include "wivar_session.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <string>

using namespace esphome::zehnder_wivar;

static int failures = 0;
static const char *current_test = "";
#define CHECK(cond)                                                                  \
  do {                                                                               \
    if (!(cond)) {                                                                   \
      std::printf("FAIL [%s] %s:%d: %s\n", current_test, __FILE__, __LINE__, #cond); \
      failures++;                                                                    \
    }                                                                                \
  } while (0)

// ── Minimal protobuf encoding for replies ────────────────────────────────────

using Bytes = std::vector<uint8_t>;

static void varint(Bytes &out, uint64_t v) {
  while (v > 0x7F) {
    out.push_back((v & 0x7F) | 0x80);
    v >>= 7;
  }
  out.push_back(v);
}
static void fvar(Bytes &out, uint32_t field, uint64_t v) {
  varint(out, field << 3);
  varint(out, v);
}
static void fbytes(Bytes &out, uint32_t field, const Bytes &b) {
  varint(out, (field << 3) | 2);
  varint(out, b.size());
  out.insert(out.end(), b.begin(), b.end());
}
static Bytes frame(uint32_t type, uint32_t result, uint32_t ref, const Bytes &body = {}) {
  Bytes op;
  fvar(op, 1, type);
  if (result)
    fvar(op, 2, result);
  fvar(op, 4, ref);
  uint32_t total = op.size() + 2 + body.size();
  Bytes f = {(uint8_t) (total >> 24), (uint8_t) (total >> 16), (uint8_t) (total >> 8), (uint8_t) total,
             (uint8_t) (op.size() >> 8), (uint8_t) op.size()};
  f.insert(f.end(), op.begin(), op.end());
  f.insert(f.end(), body.begin(), body.end());
  return f;
}

struct RoomSpec {
  int32_t temp = 215, target = 200;
  int32_t setpoints[3] = {200, 200, 180};
  uint64_t booster_end = 0;
  bool overheat = false;
  std::vector<uint32_t> device_errors;
};

static Bytes rooms_body(const RoomSpec &r) {
  Bytes dev;
  fvar(dev, 1, 0);
  fvar(dev, 7, 115);
  if (r.overheat)
    fvar(dev, 30, 1);
  for (uint32_t e : r.device_errors)
    fvar(dev, 10, e);
  Bytes room;
  fvar(room, 1, 0);
  fbytes(room, 2, Bytes{'B', 'a', 'd'});
  fvar(room, 4, r.target);
  fvar(room, 5, (uint32_t) r.temp);
  fbytes(room, 8, dev);
  for (int m = 0; m < 3; m++) {
    Bytes t;
    fvar(t, 1, m);
    fvar(t, 2, r.setpoints[m]);
    fbytes(room, 9, t);
  }
  if (r.booster_end)
    fvar(room, 16, r.booster_end);
  Bytes body;
  fbytes(body, 1, room);
  return body;
}

static Bytes run_state_body(uint32_t temp_mode, const std::vector<uint32_t> &errors = {}, uint32_t run_mode = 1) {
  Bytes rs;
  fvar(rs, 1, run_mode);
  fvar(rs, 2, temp_mode);
  for (uint32_t e : errors)
    fvar(rs, 11, e);
  Bytes body;
  fbytes(body, 1, rs);
  return body;
}

// ── Real capture ─────────────────────────────────────────────────────────────

// Replies of a real poll (remote firmware 0x100411, 2026-10-14), logged at VERBOSE level ("RX ..."):
// SetAddress, Version, GetRooms (room "Bad", 21.1 °C, target 18.0, setpoints 20.0/20.0/18.0, window closed),
// GetRunState (Automatic / Away). The device's ID string is replaced by zeros of the same length.
static const char *const REAL_POLL[] = {
    "000000080006083310002001",
    "0000001700060844100020020891884012074e6f74205365741800",
    "0000006a000708c301100020030a5f08001203426164180420b40128d30142330800120c3030203030303030303030301a074e6f742053"
    "65742000280b300038704205080110d3017801800101e00332e803004a05080010c8014a05080210b4014a05080110c80150005801680"
    "0",
    "00000017000708f101100020040a0c080110021801280030004000",
};

// A real write of the Away setpoint 18.0 -> 18.5 (same day), in a session right after REAL_POLL: the request as the
// bridge sent it (login was ref 5), and the remote's answer, a rooms update with the new values instead of a confirm.
static const char *const REAL_WRITE_REQUEST =
    "00000020000508cd0120060a1708003205080010c8013205080110c8013205080210b901";
static const char *const REAL_WRITE_REPLY =
    "00000068000508c40110000a5f08001203426164180420b90128d20142330800120c3030203030303030303030301a074e6f742053657420"
    "00280b300038724205080110d2017801800101e00332e803004a05080010c8014a05080210b9014a05080110c801500058016800";

static Bytes unhex(const char *s) {
  Bytes v;
  for (; s[0] && s[1]; s += 2) {
    unsigned b;
    std::sscanf(s, "%2x", &b);
    v.push_back(b);
  }
  return v;
}

// ── Fake remote + harness ────────────────────────────────────────────────────

struct Request {
  uint32_t op;
  uint32_t ref;
  Bytes body;
};

class Harness : public Transport, public Clock, public SessionListener {
 public:
  // Behaviour knobs.
  bool connects = true;          // connect() leads to on_connected()
  bool push_first_chunk = true;  // the remote pushes the first chunk without a read
  bool drop_link_on_rooms = false;
  uint32_t firmware = TESTED_REMOTE_FIRMWARE;
  RoomSpec room;
  uint32_t temp_mode = 0;
  std::vector<uint32_t> run_state_errors;
  // Per-op override: return the reply frames (empty = no answer).
  std::map<uint32_t, std::function<std::vector<Bytes>(const Request &)>> handlers;

  // Recorded.
  std::vector<Request> requests;
  std::vector<Bytes> sent_frames;
  std::vector<BridgeStatus> statuses;
  std::vector<std::string> status_texts;
  int connects_called = 0, disconnects = 0, rooms_seen = 0, run_states_seen = 0, errors_calls = 0, pokes = 0;
  std::vector<uint32_t> last_errors;
  int version_calls = 0;
  bool version_tested = false;
  std::vector<SessionStats> session_ends;

  uint32_t now_ms = 0;
  uint64_t unix = 1'800'000'000;
  SessionConfig cfg;
  Session session{*this, *this, *this, cfg};

  Harness() {
    session.config().boot_delay_ms = 0;
    session.config().session_gap_ms = 5000;  // shorter than the default; test_session_gap checks the default
    session.config().want_schedule = false;  // enabled by the schedule tests
    session.config().check_time = false;     // enabled by the clock tests
  }

  // Transport
  void connect() override {
    connects_called++;
    connecting_ = true;
    idle_ = false;
    if (connects)
      later_([this] { connecting_ = false; session.on_connected(); }, 300);
  }
  void send(const Bytes &f, bool secret) override {
    proto::Frame parsed;
    if (!proto::parse_frame(f.data(), f.size(), parsed)) {
      std::printf("bad request frame\n");
      failures++;
      return;
    }
    // Login and pairing carry the app UUID: the transport must be told not to log them, and only them.
    bool has_uuid = parsed.op_type == proto::OP_SET_ADDRESS_REQUEST || parsed.op_type == proto::OP_APP_PAIRING_REQUEST;
    if (secret != has_uuid) {
      std::printf("FAIL [%s] op %u sent with secret=%d\n", current_test, (unsigned) parsed.op_type, (int) secret);
      failures++;
    }
    Request req{parsed.op_type, parsed.reference, Bytes(parsed.body, parsed.body + parsed.body_len)};
    requests.push_back(req);
    sent_frames.push_back(f);
    later_([this] { session.on_sent(); }, 20);
    std::vector<Bytes> replies = reply_(req);
    for (auto &r : replies)
      queue_reply_(r);
  }
  void poke() override {
    pokes++;
    later_([this] {
      if (!pending_chunks_.empty()) {
        Bytes c = pending_chunks_.front();
        pending_chunks_.erase(pending_chunks_.begin());
        session.on_data(c.data(), c.size());
      }
      session.on_read_done();
    }, 15);
  }
  void disconnect() override {
    if (idle_)
      return;
    disconnects++;
    pending_chunks_.clear();
    later_([this] { idle_ = true; session.on_disconnected(); }, 50);
  }
  bool idle() const override { return idle_; }

  // Clock
  uint32_t millis() const override { return now_ms; }
  uint64_t unix_now() const override { return unix + now_ms / 1000; }
  uint32_t random_u32() override { return 0x12345678; }
  bool dst_known = true;
  bool dst_info(uint64_t, bool &summer, std::vector<proto::DstSwitch> &switches) override {
    if (!dst_known)
      return false;
    summer = true;
    switches = {{1'811'000'000, false}, {1'827'000'000, true}};
    return true;
  }

  // Listener
  void on_status(BridgeStatus s, const std::string &text) override {
    statuses.push_back(s);
    status_texts.push_back(text);
  }
  void on_room(const proto::Room &) override { rooms_seen++; }
  void on_run_state(const proto::RunState &) override { run_states_seen++; }
  void on_errors(const std::vector<uint32_t> &codes) override {
    errors_calls++;
    last_errors = codes;
  }
  void on_version(uint32_t, bool tested) override {
    version_calls++;
    version_tested = tested;
  }
  void on_session_end(const SessionStats &stats) override { session_ends.push_back(stats); }
  void on_schedule(const Schedule &s) override {
    schedules_seen++;
    last_schedule = s;
  }
  void on_clock_offset(int64_t seconds) override { clock_offsets.push_back(seconds); }
  void on_schedule_written(ScheduleWriteResult result, const std::string &detail) override {
    schedule_writes.push_back({result, detail});
  }
  std::vector<std::pair<ScheduleWriteResult, std::string>> schedule_writes;
  int schedules_seen = 0;
  Schedule last_schedule;
  std::vector<int64_t> clock_offsets;
  void on_log(char, const std::string &) override {}

  /// Advance simulated time, driving the session loop and delivering queued events.
  void run(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 10) {
      now_ms += 10;
      for (size_t i = 0; i < events_.size();) {
        if ((int32_t) (now_ms - events_[i].at) >= 0) {
          auto fn = events_[i].fn;
          events_.erase(events_.begin() + i);
          fn();
        } else {
          i++;
        }
      }
      session.loop();
    }
  }

  std::vector<uint32_t> ops() const {
    std::vector<uint32_t> out;
    for (auto &r : requests)
      out.push_back(r.op);
    return out;
  }
  /// A reply that arrives `delay` ms from now, if the link is still up then.
  void reply_later(const Bytes &f, uint32_t delay) {
    later_([this, f] {
      if (!idle_)
        queue_reply_(f);
    }, delay);
  }
  bool saw(BridgeStatus s) const {
    for (auto x : statuses)
      if (x == s)
        return true;
    return false;
  }
  void clear() {
    requests.clear();
    sent_frames.clear();
    statuses.clear();
    status_texts.clear();
  }

 private:
  struct Event {
    uint32_t at;
    std::function<void()> fn;
  };
  std::vector<Event> events_;
  std::vector<Bytes> pending_chunks_;
  bool idle_ = true;
  bool connecting_ = false;

  void later_(std::function<void()> fn, uint32_t delay) { events_.push_back({now_ms + delay, std::move(fn)}); }

  void queue_reply_(const Bytes &f) {
    // Split into 20-byte chunks: the first is pushed (if configured), the rest come one per read.
    for (size_t i = 0; i < f.size(); i += 20)
      pending_chunks_.push_back(Bytes(f.begin() + i, f.begin() + std::min(f.size(), i + 20)));
    if (push_first_chunk) {
      later_([this] {
        if (!pending_chunks_.empty()) {
          Bytes c = pending_chunks_.front();
          pending_chunks_.erase(pending_chunks_.begin());
          session.on_data(c.data(), c.size());
        }
      }, 80);
    }
  }

  std::vector<Bytes> reply_(const Request &req) {
    auto it = handlers.find(req.op);
    if (it != handlers.end())
      return it->second(req);
    switch (req.op) {
      case proto::OP_SET_ADDRESS_REQUEST:
        return {frame(proto::OP_SET_ADDRESS_CONFIRM, 0, req.ref)};
      case proto::OP_VERSION_REQUEST: {
        Bytes body;
        fvar(body, 1, firmware);
        return {frame(proto::OP_VERSION_CONFIRM, 0, req.ref, body)};
      }
      case proto::OP_GET_ROOMS_REQUEST:
        if (drop_link_on_rooms) {
          disconnect();
          return {};
        }
        return {frame(proto::OP_GET_ROOMS_CONFIRM, 0, req.ref, rooms_body(room))};
      case proto::OP_GET_RUN_STATE_REQUEST:
        return {frame(proto::OP_GET_RUN_STATE_CONFIRM, 0, req.ref, run_state_body(temp_mode, run_state_errors))};
      default:
        return {};
    }
  }
};

// Parse the setpoints carried by a SetRoomValue request.
static std::vector<int32_t> written_setpoints(const Request &r) {
  std::vector<proto::Field> top, rv;
  proto::decode_message(r.body.data(), r.body.size(), top);
  proto::decode_message(top[0].data, top[0].len, rv);
  std::vector<int32_t> sp(3, -1);
  for (auto &f : rv) {
    if (f.number != 6)
      continue;
    std::vector<proto::Field> t;
    proto::decode_message(f.data, f.len, t);
    uint64_t mode = 0, value = 0;
    for (auto &g : t)
      (g.number == 1 ? mode : value) = g.value;
    if (mode < 3)
      sp[mode] = value;
  }
  return sp;
}

static uint32_t first_op(const Harness &h) { return h.requests.empty() ? 0 : h.requests.front().op; }

// ── Tests ────────────────────────────────────────────────────────────────────

static void test_poll_happy_path() {
  current_test = "poll";
  Harness h;
  h.session.poll();
  h.run(5000);
  CHECK((h.ops() == std::vector<uint32_t>{1, 18, 240, 194}));
  CHECK(h.session.status() == BridgeStatus::OK);
  CHECK(h.saw(BridgeStatus::CONNECTING) && h.saw(BridgeStatus::READING));
  CHECK(h.rooms_seen == 1 && h.run_states_seen == 1);
  CHECK(h.errors_calls == 1 && h.last_errors.empty());  // once per poll
  CHECK(h.version_calls == 1 && h.version_tested);
  CHECK(h.disconnects == 1 && !h.session.active());
  CHECK(std::fabs(h.session.room().measured_temperature - 21.5f) < 0.01f);
  // Second poll: version not asked again.
  h.clear();
  h.session.poll();
  h.run(5000);
  CHECK((h.ops() == std::vector<uint32_t>{1, 240, 194}));
}

static void test_boot_delay() {
  current_test = "boot delay";
  Harness h;
  h.session.config().boot_delay_ms = 15000;
  h.session.poll();
  h.run(10000);
  CHECK(h.connects_called == 0);
  h.run(10000);
  CHECK(h.connects_called == 1 && h.session.status() == BridgeStatus::OK);
}

static void test_reply_needs_poke() {
  current_test = "poke";
  Harness h;
  h.push_first_chunk = false;  // remote only answers when read
  h.session.poll();
  h.run(15000);
  CHECK(h.session.status() == BridgeStatus::OK);
  CHECK(h.rooms_seen == 1);
  CHECK(h.pokes > 5);
}

static void test_unreachable_retries() {
  current_test = "unreachable";
  Harness h;
  h.connects = false;
  h.session.poll();
  h.run(41000);
  // Failures that will be retried are logged only: the status goes back to the last settled one.
  CHECK(h.session.status() == BridgeStatus::WAITING && !h.saw(BridgeStatus::UNREACHABLE));
  CHECK(h.connects_called == 1 && h.session.stats().failures == 1);
  h.run(31000);  // retry after 30 s
  CHECK(h.connects_called == 2);
  h.run(4 * 71000);
  CHECK(h.connects_called == 4);  // first try + 3 retries, then wait for the next poll
  CHECK(h.session.status() == BridgeStatus::UNREACHABLE);  // shown once the retries are used up
  CHECK(h.session.stats().failures == 4);
}

static void test_failed_connect_is_quiet_when_retry_works() {
  current_test = "quiet retry";
  Harness h;
  h.session.poll();
  h.run(5000);
  CHECK(h.session.status() == BridgeStatus::OK);
  h.clear();
  h.connects = false;  // one missed connect, as seen about once in 7 polls
  h.session.poll();
  h.run(41000);
  h.connects = true;
  h.run(31000);
  CHECK(h.session.status() == BridgeStatus::OK && h.connects_called == 3);
  for (auto s : h.statuses)
    CHECK(!bridge_status_is_problem(s));  // never shown as a problem
}

static void test_setpoint_write_via_rooms_update() {
  current_test = "write via rooms update";
  Harness h;
  h.session.poll();
  h.run(5000);
  h.clear();
  // The remote answers the write with a rooms update carrying the new values.
  h.handlers[proto::OP_SET_ROOM_VALUE_REQUEST] = [&h](const Request &r) {
    auto sp = written_setpoints(r);
    for (int m = 0; m < 3; m++)
      h.room.setpoints[m] = sp[m];
    return std::vector<Bytes>{frame(proto::OP_ROOMS_UPDATE_INDICATION, 0, 0, rooms_body(h.room))};
  };
  h.session.set_setpoint(2, 185);
  h.run(3000);
  CHECK((h.ops() == std::vector<uint32_t>{1, 240, 194, 205}));  // current setpoints read first
  CHECK((written_setpoints(h.requests[3]) == std::vector<int32_t>{200, 200, 185}));
  CHECK(h.session.status() == BridgeStatus::WRITTEN);
  CHECK(h.session.pending_setpoint(2) == -1);
  // The update is the read-back (as in the app): published at once, no extra session.
  CHECK(h.session.room().setpoints[2] == 185);
  CHECK(h.rooms_seen == 3);
  h.clear();
  h.run(60000);
  CHECK(h.ops().empty());
  CHECK(h.session.stats().connections == 2);
}

static void test_readback_only_without_room_data() {
  current_test = "read-back without room data";
  {
    Harness h;  // plain confirm, no values: read back
    h.session.poll();
    h.run(5000);
    h.handlers[proto::OP_SET_ROOM_VALUE_REQUEST] = [](const Request &r) {
      return std::vector<Bytes>{frame(proto::OP_SET_ROOM_VALUE_CONFIRM, 0, r.ref)};
    };
    h.session.set_setpoint(0, 210);
    h.run(3000);
    CHECK(h.session.status() == BridgeStatus::WRITTEN);
    h.clear();
    h.run(13000);
    CHECK((h.ops() == std::vector<uint32_t>{1, 240, 194}));
  }
  {
    Harness h;  // setpoints confirmed with data, but a timer in the same session: read back for the timer
    h.session.poll();
    h.run(5000);
    h.handlers[proto::OP_SET_ROOM_VALUE_REQUEST] = [&h](const Request &r) {
      auto sp = written_setpoints(r);
      for (int m = 0; m < 3; m++)
        h.room.setpoints[m] = sp[m];
      return std::vector<Bytes>{frame(proto::OP_ROOMS_UPDATE_INDICATION, 0, 0, rooms_body(h.room))};
    };
    h.handlers[proto::OP_SET_DEVICE_FUNCTION_MODE_REQUEST] = [&h](const Request &r) {
      h.room.booster_end = h.unix_now() + 60 * 60;
      return std::vector<Bytes>{frame(proto::OP_SET_DEVICE_FUNCTION_MODE_CONFIRM, 0, r.ref)};
    };
    h.clear();
    h.session.set_setpoint(2, 185);
    h.session.start_timer(60);
    h.run(3000);
    CHECK((h.ops() == std::vector<uint32_t>{1, 240, 194, 205, 334}));  // read, setpoints, then the timer
    h.clear();
    h.run(13000);
    CHECK((h.ops() == std::vector<uint32_t>{1, 240, 194}));
    CHECK(h.session.timer_remaining_s() > 59 * 60);
  }
}

static void test_readback_retried_when_remote_busy() {
  current_test = "read-back retried";
  // Seen on 2026-10-14: right after a write the remote didn't come up for the read-back; the retry worked.
  Harness h;
  h.session.poll();
  h.run(5000);
  h.handlers[proto::OP_SET_DEVICE_FUNCTION_MODE_REQUEST] = [&h](const Request &r) {
    h.room.booster_end = h.unix_now() + 30 * 60;
    h.connects = false;  // busy relaying to the heater
    return std::vector<Bytes>{frame(proto::OP_SET_DEVICE_FUNCTION_MODE_CONFIRM, 0, r.ref)};
  };
  h.session.start_timer(30);
  h.run(3000);
  CHECK(h.session.status() == BridgeStatus::WRITTEN);
  h.run(10000 + 41000);  // read-back attempt times out; retried without showing a problem
  CHECK(h.session.status() == BridgeStatus::WRITTEN && !h.saw(BridgeStatus::UNREACHABLE));
  h.connects = true;
  h.clear();
  h.run(31000);  // retry
  CHECK(h.session.status() == BridgeStatus::OK);
  CHECK((h.ops() == std::vector<uint32_t>{1, 240, 194}));
  CHECK(h.session.timer_remaining_s() > 28 * 60);  // ~80 s after the start
}

static void test_per_mode_setpoint_limits() {
  current_test = "per-mode limits";
  Harness h;
  h.session.set_setpoint(0, 120);  // Home: 15.0-28.0
  CHECK(h.session.pending_setpoint(0) == 150);
  h.session.set_setpoint(2, 280);  // Away: 10.0-25.0
  CHECK(h.session.pending_setpoint(2) == 250);
  h.session.set_setpoint(2, 70);
  CHECK(h.session.pending_setpoint(2) == 100);
  h.session.set_setpoint(1, 70);  // Asleep: not offered by the app, 10.0-28.0
  CHECK(h.session.pending_setpoint(1) == 100);
  // The configured range narrows further.
  h.session.config().max_setpoint_tenths = 220;
  h.session.set_setpoint(0, 280);
  CHECK(h.session.pending_setpoint(0) == 220);
}

static void test_overheat_alarm() {
  current_test = "overheat";
  Harness h;
  h.room.overheat = true;
  h.session.poll();
  h.run(5000);
  CHECK(h.session.room().has_overheat_alarm && h.session.room().overheat_alarm);
  h.room.overheat = false;
  h.session.poll();
  h.run(5000);
  CHECK(!h.session.room().overheat_alarm);
}

static void test_unrelated_rooms_update_ignored() {
  current_test = "unrelated rooms update";
  {
    Harness h;
    h.session.poll();
    h.run(5000);
    // Only a rooms update with the old values arrives: that is not a confirmation, keep waiting.
    h.handlers[proto::OP_SET_ROOM_VALUE_REQUEST] = [&h](const Request &) {
      return std::vector<Bytes>{frame(proto::OP_ROOMS_UPDATE_INDICATION, 0, 0, rooms_body(h.room))};
    };
    h.session.set_setpoint(0, 210);
    h.run(3000);
    CHECK(h.session.status() == BridgeStatus::WRITING);
    h.run(20000);  // write timeout: treated as unconfirmed, checked by the read-back
    CHECK(h.session.status() == BridgeStatus::WRITTEN);
  }
  {
    Harness h;  // stale update first, then the real confirm
    h.session.poll();
    h.run(5000);
    h.handlers[proto::OP_SET_ROOM_VALUE_REQUEST] = [&h](const Request &r) {
      Bytes stale = frame(proto::OP_ROOMS_UPDATE_INDICATION, 0, 0, rooms_body(h.room));
      return std::vector<Bytes>{stale, frame(proto::OP_SET_ROOM_VALUE_CONFIRM, 0, r.ref)};
    };
    h.session.set_setpoint(0, 210);
    h.run(5000);
    CHECK(h.session.status() == BridgeStatus::WRITTEN);
    CHECK(h.session.pending_setpoint(0) == -1);
  }
}

static void test_write_postponed_until_rooms_known() {
  current_test = "postponed write";
  Harness h;
  h.handlers[proto::OP_SET_ROOM_VALUE_REQUEST] = [](const Request &r) {
    return std::vector<Bytes>{frame(proto::OP_SET_ROOM_VALUE_CONFIRM, 0, r.ref)};
  };
  h.session.set_setpoint(1, 190);  // before anything was read
  h.run(3000);
  CHECK((h.ops() == std::vector<uint32_t>{1, 18, 240, 194, 205}));
  CHECK((written_setpoints(h.requests[4]) == std::vector<int32_t>{200, 190, 180}));
  CHECK(h.session.status() == BridgeStatus::WRITTEN);
}

static void test_timer_accepted_then_reported() {
  current_test = "timer accepted";
  Harness h;
  h.session.poll();
  h.run(5000);
  h.handlers[proto::OP_SET_DEVICE_FUNCTION_MODE_REQUEST] = [&h](const Request &r) {
    h.room.booster_end = h.unix_now() + 30 * 60;  // the remote starts it
    return std::vector<Bytes>{frame(proto::OP_SET_DEVICE_FUNCTION_MODE_CONFIRM, 0, r.ref)};
  };
  h.session.start_timer(30);
  h.run(3000);
  CHECK(h.session.status() == BridgeStatus::WRITTEN);
  uint32_t local = h.session.timer_remaining_s();
  CHECK(local > 29 * 60 && local <= 30 * 60);  // local countdown before the read-back
  h.run(13000);                                 // read-back reports the end time
  uint32_t remote = h.session.timer_remaining_s();
  CHECK(remote > 29 * 60 && remote <= 30 * 60);
  // Stop it.
  h.handlers[proto::OP_SET_DEVICE_FUNCTION_MODE_REQUEST] = [&h](const Request &r) {
    h.room.booster_end = 0;
    return std::vector<Bytes>{frame(proto::OP_SET_DEVICE_FUNCTION_MODE_CONFIRM, 0, r.ref)};
  };
  h.session.start_timer(0);
  h.run(3000);
  CHECK(h.session.timer_remaining_s() == 0);
}

static void test_timer_replaced() {
  current_test = "timer replaced";
  Harness h;
  h.handlers[proto::OP_SET_DEVICE_FUNCTION_MODE_REQUEST] = [&h](const Request &r) {
    h.room.booster_end = h.unix_now() + 60 * 60;
    return std::vector<Bytes>{frame(proto::OP_SET_DEVICE_FUNCTION_MODE_CONFIRM, 0, r.ref)};
  };
  h.session.poll();
  h.run(5000);
  h.session.start_timer(60);
  h.run(16000);  // read-back: the remote's end time is now known
  CHECK(h.session.timer_remaining_s() > 59 * 60);
  h.handlers[proto::OP_SET_DEVICE_FUNCTION_MODE_REQUEST] = [&h](const Request &r) {
    h.room.booster_end = h.unix_now() + 15 * 60;
    return std::vector<Bytes>{frame(proto::OP_SET_DEVICE_FUNCTION_MODE_CONFIRM, 0, r.ref)};
  };
  h.run(20000);  // session gap
  h.session.start_timer(15);
  h.run(3000);
  // Before the read-back: the new countdown, not the replaced timer's end time.
  uint32_t left = h.session.timer_remaining_s();
  CHECK(h.session.status() == BridgeStatus::WRITTEN && left > 14 * 60 && left <= 15 * 60);
}

static void test_timer_rejected() {
  current_test = "timer rejected";
  Harness h;
  h.session.poll();
  h.run(5000);
  h.handlers[proto::OP_SET_DEVICE_FUNCTION_MODE_REQUEST] = [](const Request &r) {
    return std::vector<Bytes>{frame(proto::OP_SET_DEVICE_FUNCTION_MODE_CONFIRM, 14, r.ref)};
  };
  h.session.start_timer(60);
  h.run(3000);
  CHECK(h.session.timer_remaining_s() == 0);  // no countdown for a rejected timer
}

static void test_timer_stopped_on_remote() {
  current_test = "timer stopped on remote";
  Harness h;
  h.session.poll();
  h.run(5000);
  // Confirmed, but the remote never reports an end time (e.g. stopped on the remote right away).
  h.handlers[proto::OP_SET_DEVICE_FUNCTION_MODE_REQUEST] = [](const Request &r) {
    return std::vector<Bytes>{frame(proto::OP_SET_DEVICE_FUNCTION_MODE_CONFIRM, 0, r.ref)};
  };
  h.session.start_timer(120);
  h.run(3000);
  CHECK(h.session.timer_remaining_s() > 0);
  h.run(13000);  // first read-back: grace
  CHECK(h.session.timer_remaining_s() > 0);
  h.session.poll();
  h.run(5000);  // second read without an end time: local countdown dropped
  CHECK(h.session.timer_remaining_s() == 0);
}

static void test_disconnect_mid_session_retries() {
  current_test = "disconnect mid-session";
  Harness h;
  h.drop_link_on_rooms = true;
  h.session.poll();
  h.run(5000);
  CHECK(h.session.status() == BridgeStatus::WAITING && !h.saw(BridgeStatus::DISCONNECTED));
  CHECK(!h.session.active() && h.session.stats().failures == 1);
  h.drop_link_on_rooms = false;
  h.run(31000);
  CHECK(h.connects_called == 2 && h.session.status() == BridgeStatus::OK);
}

static void test_pairing() {
  current_test = "pairing";
  {
    Harness h;
    h.handlers[proto::OP_APP_PAIRING_REQUEST] = [](const Request &r) {
      return std::vector<Bytes>{frame(proto::OP_APP_PAIRING_CONFIRM, 0, r.ref)};
    };
    h.session.start_pairing();
    h.run(6000);
    CHECK(h.saw(BridgeStatus::PAIRING_CONNECTING) && h.saw(BridgeStatus::PAIRING) && h.saw(BridgeStatus::PAIRED));
    CHECK(first_op(h) == proto::OP_APP_PAIRING_REQUEST);
    CHECK(h.session.status() == BridgeStatus::OK);
  }
  {
    Harness h;  // nobody presses MODE
    h.session.start_pairing();
    h.run(40000);
    CHECK(h.session.status() == BridgeStatus::PAIRING_FAILED);
    CHECK(h.session.status_text() == "Pairing timed out");
    h.run(60000);
    CHECK(h.connects_called == 1);  // no retry after a pairing outcome
  }
  {
    Harness h;  // remote not reachable: the request is dropped, not kept for later
    h.connects = false;
    h.session.start_pairing();
    h.run(41000);
    CHECK(h.session.status() == BridgeStatus::PAIRING_FAILED);
    CHECK(h.session.status_text() == "Pairing failed: Wivar not reachable");
    h.run(60000);
    CHECK(h.connects_called == 1);  // a pairing outcome is not retried
    h.connects = true;
    h.session.poll();  // the next normal poll must not pair
    h.run(5000);
    CHECK(first_op(h) == proto::OP_SET_ADDRESS_REQUEST);
    CHECK(h.session.status() == BridgeStatus::OK);
  }
}

static void test_not_paired() {
  current_test = "not paired";
  Harness h;
  h.handlers[proto::OP_GET_ROOMS_REQUEST] = [](const Request &r) {
    return std::vector<Bytes>{frame(proto::OP_GET_ROOMS_CONFIRM, proto::RESULT_NOT_PAIRED, r.ref)};
  };
  h.session.poll();
  h.run(5000);
  CHECK(h.session.status() == BridgeStatus::NOT_PAIRED);
  CHECK(bridge_status_is_problem(h.session.status()));
  h.run(60000);
  CHECK(h.connects_called == 1);  // not retried
}

static void test_untested_firmware() {
  current_test = "untested firmware";
  Harness h;
  h.firmware = 0x100500;
  h.session.poll();
  h.run(5000);
  CHECK(h.version_calls == 1 && !h.version_tested);
}

static void test_battery_low_from_run_state() {
  current_test = "battery low";
  Harness h;
  h.run_state_errors = {proto::DEVICE_ERROR_NONE, proto::DEVICE_ERROR_BATTERY_LOW};
  h.room.device_errors = {proto::DEVICE_ERROR_BATTERY_LOW, 3};
  h.session.poll();
  h.run(5000);
  CHECK(h.errors_calls == 1);
  CHECK((h.last_errors == std::vector<uint32_t>{proto::DEVICE_ERROR_BATTERY_LOW, 3}));
}

static void test_millis_wrap() {
  current_test = "millis wrap";
  Harness h;
  h.now_ms = 0xFFFFFFFFu - 2000;  // session starts just before the wrap
  h.session.poll();
  h.run(6000);
  CHECK(h.session.status() == BridgeStatus::OK);
}

static void test_active_target_follows_mode() {
  current_test = "active target";
  Harness h;
  h.temp_mode = 2;  // Away active
  h.handlers[proto::OP_SET_ROOM_VALUE_REQUEST] = [](const Request &r) {
    return std::vector<Bytes>{frame(proto::OP_SET_ROOM_VALUE_CONFIRM, 0, r.ref)};
  };
  h.session.poll();
  h.run(5000);
  h.clear();
  h.session.set_active_target(170);
  h.run(3000);
  CHECK((h.ops() == std::vector<uint32_t>{1, 240, 194, 205}));
  CHECK((written_setpoints(h.requests[3]) == std::vector<int32_t>{200, 200, 170}));
}

// The schedule switches the active mode after the last poll: the thermostat target goes to the mode active now.
static void test_active_target_after_mode_switch() {
  current_test = "active target after mode switch";
  for (uint32_t before : {0u, 2u}) {
    Harness h;
    h.temp_mode = before;
    h.handlers[proto::OP_SET_ROOM_VALUE_REQUEST] = [](const Request &r) {
      return std::vector<Bytes>{frame(proto::OP_SET_ROOM_VALUE_CONFIRM, 0, r.ref)};
    };
    h.session.poll();
    h.run(5000);
    CHECK(h.session.temp_mode() == (int) before);
    h.temp_mode = before == 0 ? 2 : 0;
    h.clear();
    h.session.set_active_target(190);
    h.run(3000);
    CHECK((h.ops() == std::vector<uint32_t>{1, 240, 194, 205}));
    if (h.requests.size() == 4) {
      auto expected = before == 0 ? std::vector<int32_t>{200, 200, 190} : std::vector<int32_t>{190, 200, 180};
      CHECK(written_setpoints(h.requests[3]) == expected);
    }
  }
}

static void test_session_stats() {
  current_test = "session stats";
  Harness h;
  h.session.poll();
  h.run(5000);
  CHECK(h.session_ends.size() == 1);
  SessionStats first = h.session_ends.back();
  CHECK(first.connections == 1 && first.failures == 0);
  // Connected from on_connected until the disconnect event: a few hundred ms with the fake remote.
  CHECK(first.last_connected_ms > 0 && first.last_connected_ms < 3000);
  CHECK(first.total_connected_ms == first.last_connected_ms);
  h.session.poll();
  h.run(5000);
  CHECK(h.session_ends.size() == 2 && h.session.stats().connections == 2);
  CHECK(h.session.stats().total_connected_ms == first.last_connected_ms + h.session.stats().last_connected_ms);
  // A connect that never comes up is a failure, not a connection.
  h.connects = false;
  h.session.poll();
  h.run(46000);  // after the 5 s gap and the 40 s connect timeout
  CHECK(h.session_ends.size() == 2);
  CHECK(h.session.stats().connections == 2 && h.session.stats().failures == 1);
}

static void test_configured_retries_and_timeout() {
  current_test = "configured retries";
  Harness h;
  h.connects = false;
  h.session.config().connect_timeout_ms = 20000;
  h.session.config().retry_delay_ms = 10000;
  h.session.config().max_retries = 1;
  h.session.poll();
  h.run(21000);
  CHECK(h.session.status() != BridgeStatus::UNREACHABLE);  // a retry is still to come
  h.run(11000);
  CHECK(h.connects_called == 2);
  h.run(120000);
  CHECK(h.connects_called == 2);  // one retry only
  CHECK(h.session.status() == BridgeStatus::UNREACHABLE);
}

static void test_configured_setpoint_range() {
  current_test = "setpoint range";
  Harness h;
  h.session.config().max_setpoint_tenths = 240;
  h.handlers[proto::OP_SET_ROOM_VALUE_REQUEST] = [](const Request &r) {
    return std::vector<Bytes>{frame(proto::OP_SET_ROOM_VALUE_CONFIRM, 0, r.ref)};
  };
  h.session.poll();
  h.run(5000);
  h.clear();
  h.session.set_setpoint(0, 280);
  CHECK(h.session.pending_setpoint(0) == 240);
  h.run(3000);
  CHECK(h.ops().size() == 4 && (written_setpoints(h.requests[3]) == std::vector<int32_t>{240, 200, 180}));
}

static void test_real_replay() {
  current_test = "real poll and write replay";
  Harness h;
  const uint32_t ops[] = {proto::OP_SET_ADDRESS_REQUEST, proto::OP_VERSION_REQUEST, proto::OP_GET_ROOMS_REQUEST,
                          proto::OP_GET_RUN_STATE_REQUEST};
  for (int i = 0; i < 4; i++) {
    Bytes reply = unhex(REAL_POLL[i]);
    h.handlers[ops[i]] = [reply](const Request &) { return std::vector<Bytes>{reply}; };
  }
  h.push_first_chunk = false;  // as on the device: every chunk needs a read
  h.session.poll();
  h.run(15000);
  CHECK(h.session.status() == BridgeStatus::OK);
  CHECK((h.ops() == std::vector<uint32_t>{1, 18, 240, 194}));
  CHECK(h.version_calls == 1 && h.version_tested);
  const proto::Room &r = h.session.room();
  CHECK(r.name == "Bad");
  CHECK(std::fabs(r.measured_temperature - 21.1f) < 0.01f && std::fabs(r.target_temperature - 18.0f) < 0.01f);
  CHECK(r.setpoints[0] == 200 && r.setpoints[1] == 200 && r.setpoints[2] == 180);
  CHECK(!r.open_window_detected);
  CHECK(r.has_fluid_temperature && r.fluid_temperature == 50);
  CHECK(h.session.temp_mode() == 2);  // Away
  CHECK(h.errors_calls == 1 && h.last_errors.empty());
  CHECK(h.session.stats().connections == 1);

  // The real write, answered like the remote did.
  h.handlers[proto::OP_SET_ROOM_VALUE_REQUEST] = [](const Request &) {
    return std::vector<Bytes>{unhex(REAL_WRITE_REPLY)};
  };
  h.clear();
  h.session.set_setpoint(2, 185);
  h.run(8000);
  CHECK(h.session.status() == BridgeStatus::WRITTEN);
  CHECK((h.ops() == std::vector<uint32_t>{1, 240, 194, 205}));
  // Byte for byte as on the device, except the reference: the bridge now reads the setpoints before writing.
  std::string expected = REAL_WRITE_REQUEST;
  expected.replace(expected.find("2006"), 4, "2008");
  CHECK(proto::to_hex(h.sent_frames.at(3)) == expected);
  CHECK(h.session.pending_setpoint(2) == -1);
  CHECK(h.session.room().setpoints[2] == 185);  // taken from the remote's update
  CHECK(!h.session.room().overheat_alarm && h.session.room().fluid_temperature == 50);
  h.clear();
  h.run(60000);
  CHECK(h.ops().empty());  // no read-back session
}

// Day plan / week plan replies for the schedule tests.
static Bytes day_plan_reply(uint32_t ref, uint32_t id, uint32_t home_at, uint32_t away_at) {
  Bytes home, away, plan, body;
  fvar(home, 1, home_at);
  fvar(home, 2, 0);
  fvar(away, 1, away_at);
  fvar(away, 2, 2);
  fvar(plan, 1, id);
  fbytes(plan, 3, away);
  fbytes(plan, 3, home);
  fbytes(body, 1, plan);
  return frame(proto::OP_GET_DAY_PLAN_CONFIRM, 0, ref, body);
}

static void install_schedule(Harness &h) {
  h.session.config().want_schedule = true;
  h.handlers[proto::OP_GET_SCHEDULE_IDS_REQUEST] = [](const Request &r) {
    Bytes body;
    fvar(body, 1, 1);
    fvar(body, 1, 2);
    fvar(body, 2, 7);
    return std::vector<Bytes>{frame(proto::OP_GET_SCHEDULE_IDS_CONFIRM, 0, r.ref, body)};
  };
  h.handlers[proto::OP_GET_DAY_PLAN_REQUEST] = [](const Request &r) {
    uint32_t id = r.body.size() >= 2 ? r.body[1] : 0;
    return std::vector<Bytes>{id == 1 ? day_plan_reply(r.ref, 1, 7 * 60, 22 * 60)  // minutes on the wire
                                      : day_plan_reply(r.ref, 2, 9 * 60, 23 * 60)};
  };
  h.handlers[proto::OP_GET_WEEK_PLAN_REQUEST] = [](const Request &r) {
    Bytes wp, body;
    fvar(wp, 1, 7);
    for (uint32_t day = 3; day <= 9; day++)
      fvar(wp, day, day <= 7 ? 1 : 2);
    fbytes(body, 1, wp);
    return std::vector<Bytes>{frame(proto::OP_GET_WEEK_PLAN_CONFIRM, 0, r.ref, body)};
  };
}

static void test_schedule_read() {
  current_test = "schedule";
  Harness h;
  install_schedule(h);
  h.session.poll();
  h.run(8000);
  CHECK(h.session.status() == BridgeStatus::OK);
  CHECK((h.ops() == std::vector<uint32_t>{1, 18, 240, 194, 177, 161, 161, 163}));
  CHECK(h.schedules_seen == 1 && h.session.has_schedule());
  const Schedule &s = h.last_schedule;
  CHECK(s.day_plans.size() == 2 && s.week_plans.size() == 1);
  CHECK(s.day_plan(1) && proto::describe_day_plan(*s.day_plan(1)) == "07:00 Home, 22:00 Away");
  CHECK(s.week_plan(7) && s.week_plan(7)->day_ids[0] == 1 && s.week_plan(7)->day_ids[6] == 2);
  // Not again on the next poll, but after 24 h or on request.
  h.clear();
  h.session.poll();
  h.run(5000);
  CHECK((h.ops() == std::vector<uint32_t>{1, 240, 194}));
  h.session.request_schedule();
  h.session.poll();
  h.run(8000);
  CHECK(h.schedules_seen == 2);
  h.clear();
  h.now_ms += 24 * 3600 * 1000;
  h.session.poll();
  h.run(8000);
  CHECK(h.schedules_seen == 3 && h.ops().size() == 7);
}

static void test_schedule_read_failure_keeps_previous() {
  current_test = "schedule failure";
  Harness h;
  install_schedule(h);
  h.session.poll();
  h.run(8000);
  CHECK(h.schedules_seen == 1);
  h.handlers[proto::OP_GET_WEEK_PLAN_REQUEST] = [](const Request &) { return std::vector<Bytes>{}; };  // no answer
  h.session.request_schedule();
  h.session.poll();
  h.run(20000);
  CHECK(h.session.status() == BridgeStatus::OK);  // the poll itself is fine
  CHECK(h.schedules_seen == 1 && h.session.schedule().day_plans.size() == 2);
}

static void test_run_mode_write() {
  current_test = "run mode";
  Harness h;
  Bytes last_body;
  h.handlers[proto::OP_SET_RUN_STATE_REQUEST] = [&h, &last_body](const Request &r) {
    last_body = r.body;
    return std::vector<Bytes>{frame(proto::OP_SET_RUN_STATE_CONFIRM, 0, r.ref, r.body)};
  };
  h.temp_mode = 2;  // Away (schedule)
  h.session.poll();
  h.run(5000);
  h.clear();
  h.session.set_run_mode(RunModeChoice::MANUAL);
  CHECK(h.session.pending_run_mode() == static_cast<int>(RunModeChoice::MANUAL));
  h.run(3000);
  CHECK((h.ops() == std::vector<uint32_t>{1, 243}));
  CHECK((last_body == Bytes{0x08, 0x02, 0x10, 0x02}));  // Manual, location as now (Away), like the app
  CHECK(h.session.status() == BridgeStatus::WRITTEN && h.session.pending_run_mode() == -1);
  h.temp_mode = 0;  // what the Wivar does: Manual is Home
  h.clear();
  h.run(13000);  // read-back: the new mode and its target
  CHECK((h.ops() == std::vector<uint32_t>{1, 240, 194}));
  CHECK(h.session.temp_mode() == 0);
  h.session.set_run_mode(RunModeChoice::AUTOMATIC);
  h.run(3000);
  CHECK((last_body == Bytes{0x08, 0x01, 0x10, 0x01}));
  h.session.set_run_mode(RunModeChoice::ANTIFREEZE);
  h.run(20000);
  CHECK(last_body.size() == 4 && last_body[1] == 0x03);
}

static void test_run_mode_confirmed_by_indication() {
  current_test = "run mode via indication";
  Harness h;
  h.handlers[proto::OP_SET_RUN_STATE_REQUEST] = [](const Request &) {
    return std::vector<Bytes>{frame(proto::OP_RUN_STATE_UPDATE_INDICATION, 0, 0, run_state_body(0, {}, 2))};
  };
  h.session.poll();
  h.run(5000);
  h.session.set_run_mode(RunModeChoice::MANUAL);
  h.run(3000);
  CHECK(h.session.status() == BridgeStatus::WRITTEN && h.session.pending_run_mode() == -1);
}

static void test_reply_framing_kept_across_requests() {
  current_test = "framing across requests";
  Harness h;
  // As seen on the device: the run state indication and the confirm behind it arrive as one chunked stream, so a
  // chunk holds the end of the indication and the start of the confirm. The indication completes the write and
  // the timer request goes out while the confirm is still arriving; its bytes must not be dropped.
  h.handlers[proto::OP_SET_RUN_STATE_REQUEST] = [](const Request &r) {
    Bytes stream = frame(proto::OP_RUN_STATE_UPDATE_INDICATION, 0, 0, run_state_body(0, {}, 2));
    Bytes confirm = frame(proto::OP_SET_RUN_STATE_CONFIRM, 0, r.ref, Bytes(30, 0));
    if (stream.size() % 20 == 0)
      stream.push_back(0);  // keep the frame boundary inside a chunk (no such padding is needed today)
    stream.insert(stream.end(), confirm.begin(), confirm.end());
    return std::vector<Bytes>{stream};
  };
  h.handlers[proto::OP_SET_DEVICE_FUNCTION_MODE_REQUEST] = [](const Request &r) {
    return std::vector<Bytes>{frame(proto::OP_SET_DEVICE_FUNCTION_MODE_CONFIRM, 0, r.ref)};
  };
  h.session.poll();
  h.run(5000);
  h.session.set_run_mode(RunModeChoice::MANUAL);
  h.session.start_timer(15);
  h.run(3000);
  // The timer confirm came through: no timeout and read-back fallback, and the local countdown runs.
  CHECK(h.session.status() == BridgeStatus::WRITTEN && h.session.pending_run_mode() == -1);
  CHECK(h.session.timer_remaining_s() > 14 * 60);
}

static void test_run_mode_written_before_timer() {
  current_test = "run mode before timer";
  Harness h;
  // Like the Wivar: writing the run state ends a running timer (the end time disappears from the room).
  h.handlers[proto::OP_SET_RUN_STATE_REQUEST] = [&h](const Request &r) {
    h.room.booster_end = 0;
    return std::vector<Bytes>{frame(proto::OP_SET_RUN_STATE_CONFIRM, 0, r.ref, r.body)};
  };
  h.handlers[proto::OP_SET_DEVICE_FUNCTION_MODE_REQUEST] = [&h](const Request &r) {
    h.room.booster_end = h.unix_now() + 15 * 60;
    return std::vector<Bytes>{frame(proto::OP_SET_DEVICE_FUNCTION_MODE_CONFIRM, 0, r.ref)};
  };
  h.session.poll();
  h.run(5000);
  h.clear();
  // Queued together (timer first), written in one session.
  h.session.start_timer(15);
  h.session.set_run_mode(RunModeChoice::AUTOMATIC);
  h.run(3000);
  CHECK((h.ops() == std::vector<uint32_t>{1, proto::OP_SET_RUN_STATE_REQUEST, proto::OP_SET_DEVICE_FUNCTION_MODE_REQUEST}));
  h.run(13000);  // read-back: the remote still reports the timer
  uint32_t left = h.session.timer_remaining_s();
  CHECK(h.room.booster_end != 0 && left > 14 * 60 && left <= 15 * 60);
}

static void test_clock_check_and_sync() {
  current_test = "clock";
  Harness h;
  h.session.config().check_time = true;
  Bytes set_body;
  int remote_ahead = 95;      // remote runs 95 s ahead
  uint32_t remote_dst = 2;    // and thinks it's winter
  bool remote_knows_switch = false;
  h.handlers[proto::OP_GET_TIME_REQUEST] = [&](const Request &r) {
    Bytes body;
    fvar(body, 1, h.unix_now() + remote_ahead);
    fvar(body, 3, remote_dst);
    if (remote_knows_switch) {
      Bytes sw;
      fvar(sw, 1, 1'811'000'000);
      fvar(sw, 2, 2);
      fbytes(body, 4, sw);
    }
    return std::vector<Bytes>{frame(proto::OP_GET_TIME_CONFIRM, 0, r.ref, body)};
  };
  h.handlers[proto::OP_SET_TIME_REQUEST] = [&set_body](const Request &r) {
    set_body = r.body;
    return std::vector<Bytes>{frame(proto::OP_SET_TIME_CONFIRM, 0, r.ref)};
  };
  h.session.poll();
  h.run(8000);
  CHECK(h.session.status() == BridgeStatus::OK);
  CHECK((h.ops() == std::vector<uint32_t>{1, 18, 240, 194, 260, 262}));
  CHECK(h.clock_offsets.size() == 1 && h.clock_offsets[0] >= 94 && h.clock_offsets[0] <= 96);
  std::vector<proto::Field> f;
  CHECK(proto::decode_message(set_body.data(), set_body.size(), f) && f.size() == 4);
  CHECK(f.size() == 4 && f[1].number == 2 && f[1].value == 3);  // summer
  CHECK(f.size() == 4 && f[2].number == 3 && f[3].number == 3);  // two switches
  // Once a day.
  h.clear();
  h.session.poll();
  h.run(5000);
  CHECK((h.ops() == std::vector<uint32_t>{1, 240, 194}));
  h.clear();
  h.now_ms += 24 * 3600 * 1000;
  h.session.poll();
  h.run(8000);
  CHECK((h.ops() == std::vector<uint32_t>{1, 240, 194, 260, 262}));
  // In sync (within 30 s, same daylight saving, next switch known): read only, no write.
  remote_ahead = 12;
  remote_dst = 3;
  remote_knows_switch = true;
  h.clear();
  h.now_ms += 24 * 3600 * 1000;
  h.session.poll();
  h.run(8000);
  CHECK((h.ops() == std::vector<uint32_t>{1, 240, 194, 260}));
  CHECK(h.clock_offsets.back() >= 11 && h.clock_offsets.back() <= 13);
  // Right time but the next switch missing: written.
  remote_knows_switch = false;
  h.clear();
  h.now_ms += 24 * 3600 * 1000;
  h.session.poll();
  h.run(8000);
  CHECK((h.ops() == std::vector<uint32_t>{1, 240, 194, 260, 262}));
  // Unknown timezone: read only.
  h.dst_known = false;
  h.clear();
  h.now_ms += 24 * 3600 * 1000;
  h.session.poll();
  h.run(8000);
  CHECK((h.ops() == std::vector<uint32_t>{1, 240, 194, 260}));
}

static void test_next_switch_skips_unchanged_modes() {
  current_test = "next switch, unchanged modes";
  // Shaped like a real Wivar schedule: Away at midnight, a short Home window in the morning.
  Schedule s;
  proto::DayPlan weekday, weekend;
  weekday.id = 0;
  weekday.items = {{0, 2}, {6 * 3600, 0}, {7 * 3600, 2}};
  weekend.id = 1;
  weekend.items = {{0, 2}, {8 * 3600, 0}, {9 * 3600, 2}};
  s.day_plans = {weekday, weekend};
  proto::WeekPlan week;
  for (int d = 0; d < 7; d++) {
    week.has_day[d] = true;
    week.day_ids[d] = d < 5 ? 0 : 1;
  }
  s.week_plans = {week};
  proto::RunState rs;
  rs.run_mode = proto::RUN_MODE_AUTOMATIC;
  rs.temp_mode = 2;
  rs.has_today_dayplan = true;
  rs.today_dayplan_id = 0;
  rs.has_today_weekplan = true;
  rs.today_weekplan_id = 0;
  CHECK(describe_next_switch(s, rs, 2, 3 * 3600) == "06:00 Home");
  CHECK(describe_next_switch(s, rs, 2, 6 * 3600 + 60) == "07:00 Away");
  CHECK(describe_next_switch(s, rs, 2, 8 * 3600) == "Tomorrow 06:00 Home");  // not "Tomorrow 00:00 Away"
  CHECK(describe_next_switch(s, rs, 4, 8 * 3600) == "Tomorrow 08:00 Home");  // Friday -> weekend plan
  CHECK(describe_week(s, rs) == "Mo-Fr: 00:00 Away, 06:00 Home, 07:00 Away; Sa-Su: 00:00 Away, 08:00 Home, 09:00 Away");
}

static void test_clock_without_get_time() {
  current_test = "clock without GetTime";
  // The real Wivar doesn't answer GetTime: ask once, then set the clock daily without a check.
  Harness h;
  h.session.config().check_time = true;
  int sets = 0;
  h.handlers[proto::OP_SET_TIME_REQUEST] = [&sets](const Request &r) {
    sets++;
    return std::vector<Bytes>{frame(proto::OP_SET_TIME_CONFIRM, 0, r.ref)};
  };
  h.session.poll();
  h.run(15000);
  CHECK(h.session.status() == BridgeStatus::OK);
  CHECK((h.ops() == std::vector<uint32_t>{1, 18, 240, 194, 260, 262}));
  CHECK(sets == 1 && h.clock_offsets.empty());
  h.clear();
  h.session.poll();
  h.run(5000);
  CHECK((h.ops() == std::vector<uint32_t>{1, 240, 194}));
  h.clear();
  h.now_ms += 24 * 3600 * 1000;
  h.session.poll();
  h.run(5000);
  CHECK((h.ops() == std::vector<uint32_t>{1, 240, 194, 262}));  // no second GetTime
  CHECK(sets == 2);
}

static void test_schedule_presentation() {
  current_test = "schedule presentation";
  Harness h;
  install_schedule(h);
  h.session.poll();
  h.run(8000);
  const Schedule &s = h.session.schedule();
  proto::RunState rs;
  rs.run_mode = proto::RUN_MODE_AUTOMATIC;
  rs.temp_mode = 2;  // Away before the first switch, as the remote reports it
  rs.has_today_weekplan = true;
  rs.today_weekplan_id = 7;
  CHECK(describe_week(s, rs) == "Mo-Fr: 07:00 Home, 22:00 Away; Sa-Su: 09:00 Home, 23:00 Away");
  CHECK(describe_next_switch(s, rs, 0, 6 * 3600) == "07:00 Home");             // Monday 06:00
  CHECK(describe_next_switch(s, rs, 0, 21 * 3600) == "22:00 Away");            // Monday 21:00
  CHECK(describe_next_switch(s, rs, 4, 23 * 3600) == "Tomorrow 09:00 Home");   // Friday 23:00 -> Saturday plan
  CHECK(describe_next_switch(s, rs, 6, 23 * 3600 + 60) == "Tomorrow 07:00 Home");  // Sunday night -> Monday
  CHECK(active_week(s, rs) == s.week_plan(7));
  // The remote's own "today" wins over the weekday.
  rs.has_today_dayplan = true;
  rs.today_dayplan_id = 2;
  CHECK(today_plan(s, rs, 0) == s.day_plan(2));
  rs.run_mode = proto::RUN_MODE_MANUAL;
  CHECK(describe_next_switch(s, rs, 0, 0) == "None (Manual)");
  Schedule empty;
  rs.run_mode = proto::RUN_MODE_AUTOMATIC;
  CHECK(describe_week(empty, rs) == "-" && describe_next_switch(empty, rs, 0, 0) == "-");
}

// What can go wrong with one plan write.
enum class Fault {
  NONE,
  DROP_NOT_APPLIED,  // the link drops before the remote applies it
  DROP_APPLIED,      // applied, then the link drops before the confirm
  LOST_REQUEST,      // never arrives: no confirm, the bridge times out
  LOST_CONFIRM,      // applied, the confirm gets lost
  LATE_CONFIRM,      // applied, confirmed after the bridge's timeout
  APP_EDIT,          // the app changes the schedule just before this write (after the bridge's read)
};

// A fake remote that stores its schedule and answers reads and writes from it.
struct FakeSchedule {
  std::map<uint32_t, proto::DayPlan> days;
  std::map<uint32_t, proto::WeekPlan> weeks;
  bool store_writes = true;  // false: confirm but keep the old plan
  uint32_t write_result = 0;
  bool add_stores_empty = false;  // like Wivar firmware U0.2.7: content needs a separate SetDayplan
  int day_writes = 0, week_writes = 0, day_adds = 0, day_removes = 0;
  // Fault injection: `fault` hits the plan write request with index `fault_at` (counting from 0).
  int plan_writes = 0;
  int fault_at = -1;
  Fault fault = Fault::NONE;
  std::function<void(FakeSchedule &)> app_edit;  // for Fault::APP_EDIT
  std::function<void()> on_change;               // after every write the remote applies
};

// A plan write on the fake remote: applied and answered, unless a fault is injected for it.
static std::vector<Bytes> plan_write(Harness &h, FakeSchedule &fs, const std::function<std::vector<Bytes>()> &apply) {
  Fault fault = fs.plan_writes++ == fs.fault_at ? fs.fault : Fault::NONE;
  if (fault == Fault::APP_EDIT && fs.app_edit)
    fs.app_edit(fs);
  if (fault == Fault::DROP_NOT_APPLIED || fault == Fault::LOST_REQUEST) {
    if (fault == Fault::DROP_NOT_APPLIED)
      h.disconnect();
    return {};
  }
  std::vector<Bytes> replies = apply();
  if (fs.on_change)
    fs.on_change();
  switch (fault) {
    case Fault::DROP_APPLIED:
      h.disconnect();
      return {};
    case Fault::LOST_CONFIRM:
      return {};
    case Fault::LATE_CONFIRM:
      for (auto &r : replies)
        h.reply_later(r, 20100);  // the bridge gives up after 20 s
      return {};
    default:
      return replies;
  }
}

static Bytes body_of(const Bytes &f) {
  proto::Frame parsed;
  proto::parse_frame(f.data(), f.size(), parsed);
  return Bytes(parsed.body, parsed.body + parsed.body_len);
}

static void install_fake_schedule(Harness &h, FakeSchedule &fs) {
  h.session.config().want_schedule = true;
  h.handlers[proto::OP_GET_SCHEDULE_IDS_REQUEST] = [&fs](const Request &r) {
    Bytes body;
    for (auto &d : fs.days)
      fvar(body, 1, d.first);
    for (auto &w : fs.weeks)
      fvar(body, 2, w.first);
    return std::vector<Bytes>{frame(proto::OP_GET_SCHEDULE_IDS_CONFIRM, 0, r.ref, body)};
  };
  // Get*Plan confirms and Set*Plan requests share the body layout { plan(1) }.
  h.handlers[proto::OP_GET_DAY_PLAN_REQUEST] = [&fs](const Request &r) {
    uint32_t id = r.body.size() >= 2 ? r.body[1] : 0;
    return std::vector<Bytes>{
        frame(proto::OP_GET_DAY_PLAN_CONFIRM, 0, r.ref, body_of(proto::build_set_day_plan(0, fs.days[id])))};
  };
  h.handlers[proto::OP_GET_WEEK_PLAN_REQUEST] = [&fs](const Request &r) {
    uint32_t id = r.body.size() >= 2 ? r.body[1] : 0;
    return std::vector<Bytes>{
        frame(proto::OP_GET_WEEK_PLAN_CONFIRM, 0, r.ref, body_of(proto::build_set_week_plan(0, fs.weeks[id])))};
  };
  h.handlers[proto::OP_SET_DAY_PLAN_REQUEST] = [&h, &fs](const Request &r) { return plan_write(h, fs, [&] {
    fs.day_writes++;
    proto::DayPlan plan;
    proto::parse_day_plan(r.body.data(), r.body.size(), plan);
    if (fs.store_writes && fs.write_result == 0)
      fs.days[plan.id] = plan;
    // As the real remote did (2026-10-14): rooms and run-state updates first, then the confirm.
    return std::vector<Bytes>{frame(proto::OP_ROOMS_UPDATE_INDICATION, 0, 0, rooms_body(RoomSpec{})),
                              frame(proto::OP_RUN_STATE_UPDATE_INDICATION, 0, 0, run_state_body(2)),
                              frame(proto::OP_ROOMS_UPDATE_INDICATION, 0, 0, rooms_body(RoomSpec{})),
                              frame(proto::OP_SET_DAY_PLAN_CONFIRM, fs.write_result, r.ref, r.body)};
  }); };
  h.handlers[proto::OP_ADD_DAY_PLAN_REQUEST] = [&h, &fs](const Request &r) { return plan_write(h, fs, [&] {
    fs.day_adds++;
    proto::DayPlan plan;
    proto::parse_day_plan(r.body.data(), r.body.size(), plan);
    if (fs.write_result != 0 || fs.days.size() >= 8)
      return std::vector<Bytes>{frame(proto::OP_ADD_DAY_PLAN_CONFIRM, 14, r.ref)};
    uint32_t id = 0;
    while (fs.days.count(id))
      id++;
    plan.id = id;
    if (!fs.add_stores_empty)
      fs.days[id] = plan;
    else
      fs.days[id] = proto::DayPlan{id, "", {}, {}};
    return std::vector<Bytes>{frame(proto::OP_ADD_DAY_PLAN_CONFIRM, 0, r.ref, body_of(proto::build_set_day_plan(0, fs.days[id])))};
  }); };
  h.handlers[proto::OP_REMOVE_DAY_PLAN_REQUEST] = [&h, &fs](const Request &r) { return plan_write(h, fs, [&] {
    uint32_t id = r.body.size() >= 2 ? r.body[1] : 0;
    if (!fs.days.count(id))
      return std::vector<Bytes>{frame(proto::OP_REMOVE_DAY_PLAN_CONFIRM, 14, r.ref)};  // no such plan: rejected
    fs.day_removes++;
    fs.days.erase(id);
    Bytes body;
    fvar(body, 1, id);
    return std::vector<Bytes>{frame(proto::OP_REMOVE_DAY_PLAN_CONFIRM, 0, r.ref, body)};
  }); };
  h.handlers[proto::OP_SET_WEEK_PLAN_REQUEST] = [&h, &fs](const Request &r) { return plan_write(h, fs, [&] {
    fs.week_writes++;
    proto::WeekPlan plan;
    proto::parse_week_plan(r.body.data(), r.body.size(), plan);
    if (fs.store_writes && fs.write_result == 0)
      fs.weeks[plan.id] = plan;
    return std::vector<Bytes>{frame(proto::OP_SET_WEEK_PLAN_CONFIRM, fs.write_result, r.ref, r.body)};
  }); };
}

static FakeSchedule default_fake_schedule() {
  FakeSchedule fs;
  proto::DayPlan weekday, weekend;
  weekday.id = 0;
  weekday.name = "WD";
  weekday.items = {{0, 2}, {6 * 3600, 0}, {7 * 3600, 2}};
  weekend.id = 1;
  weekend.name = "WE";
  weekend.items = {{0, 2}, {8 * 3600, 0}, {9 * 3600, 2}};
  fs.days[0] = weekday;
  fs.days[1] = weekend;
  proto::WeekPlan week;
  week.name = "W";
  for (int d = 0; d < 7; d++) {
    week.has_day[d] = true;
    week.day_ids[d] = d < 5 ? 0 : 1;
  }
  fs.weeks[0] = week;
  return fs;
}

static void test_day_plan_write() {
  current_test = "day plan write";
  Harness h;
  FakeSchedule fs = default_fake_schedule();
  install_fake_schedule(h, fs);
  proto::DayPlan plan = fs.days[0];
  plan.items[1].start_s = 6 * 3600 + 30 * 60;  // Home from 06:30
  CHECK(h.session.write_day_plan(plan) == "schedule not read yet");
  h.session.poll();
  h.run(10000);
  CHECK(h.session.has_schedule());
  h.clear();
  CHECK(h.session.write_day_plan(plan).empty());
  CHECK(h.session.plan_writes_pending());
  h.run(3000);
  CHECK((h.ops() == std::vector<uint32_t>{1, 177, 161, 161, 163, 173}));  // planned from a read in the same session
  CHECK(h.session.status() == BridgeStatus::WRITTEN && fs.day_writes == 1);
  h.clear();
  h.run(10000);  // read-back reads the whole schedule and compares
  CHECK((h.ops() == std::vector<uint32_t>{1, 177, 161, 161, 163, 240, 194}));
  CHECK(h.schedule_writes.size() == 1 && h.schedule_writes[0].first == ScheduleWriteResult::OK);
  CHECK(!h.session.plan_writes_pending());
  CHECK(proto::describe_day_plan(*h.session.schedule().day_plan(0)) == "00:00 Away, 06:30 Home, 07:00 Away");
}

static void test_day_plan_write_checks() {
  current_test = "day plan write checks";
  Harness h;
  FakeSchedule fs = default_fake_schedule();
  install_fake_schedule(h, fs);
  h.session.poll();
  h.run(10000);
  proto::DayPlan plan = fs.days[0];
  plan.id = 5;
  CHECK(h.session.write_day_plan(plan) == "no day plan with id 5");
  plan.id = 0;
  plan.items[0].start_s = 300;
  CHECK(h.session.write_day_plan(plan) == "the first entry must start at 00:00");
  proto::WeekPlan week = fs.weeks[0];
  week.day_ids[6] = 9;
  CHECK(h.session.write_week_plan(week) == "no day plan with id 9");
  CHECK(!h.session.plan_writes_pending() && fs.day_writes == 0 && fs.week_writes == 0);
}

static void test_plan_write_not_applied() {
  current_test = "plan write not applied";
  {
    Harness h;  // confirmed, but the remote keeps the old plan: the read-back notices
    FakeSchedule fs = default_fake_schedule();
    install_fake_schedule(h, fs);
    h.session.poll();
    h.run(10000);
    fs.store_writes = false;
    proto::DayPlan plan = fs.days[1];
    plan.items[1].start_s = 7 * 3600;
    CHECK(h.session.write_day_plan(plan).empty());
    h.run(60000);  // written in three sessions, each read back
    CHECK(fs.day_writes == 3);
    CHECK(h.schedule_writes.size() == 1 && h.schedule_writes[0].first == ScheduleWriteResult::FAILED);
    CHECK(h.schedule_writes.size() == 1 && h.schedule_writes[0].second == "differs after writing: day plan 1");
  }
  {
    Harness h;  // rejected
    FakeSchedule fs = default_fake_schedule();
    install_fake_schedule(h, fs);
    h.session.poll();
    h.run(10000);
    fs.write_result = 14;
    proto::WeekPlan week = fs.weeks[0];
    week.day_ids[4] = 1;  // Friday as weekend
    CHECK(h.session.write_week_plan(week).empty());
    h.run(15000);
    CHECK(h.schedule_writes.size() == 1 && h.schedule_writes[0].first == ScheduleWriteResult::FAILED);
    CHECK(h.schedule_writes.size() == 1 && h.schedule_writes[0].second == "week plan write rejected (result 14)");
  }
}

static void test_week_and_day_plan_in_one_session() {
  current_test = "week and day plan";
  Harness h;
  FakeSchedule fs = default_fake_schedule();
  install_fake_schedule(h, fs);
  h.session.poll();
  h.run(10000);
  h.clear();
  proto::WeekPlan week = fs.weeks[0];
  week.day_ids[4] = 1;
  proto::DayPlan weekend = fs.days[1];
  weekend.items.push_back({22 * 3600, 0});
  CHECK(h.session.write_week_plan(week).empty());
  CHECK(h.session.write_day_plan(weekend) == "the previous schedule change is still being written");  // one at a time
  h.run(3000);
  CHECK((h.ops() == std::vector<uint32_t>{1, 177, 161, 161, 163, 175}));
  h.run(10000);
  CHECK(h.schedule_writes.size() == 1 && h.schedule_writes[0].first == ScheduleWriteResult::OK);
  CHECK(h.session.schedule().week_plan(0)->day_ids[4] == 1);
  CHECK(h.session.write_day_plan(weekend).empty());  // now it's accepted
}

static std::string week_text(const Schedule &s, uint32_t week_id = 0) {
  proto::RunState rs;
  rs.has_today_weekplan = true;
  rs.today_weekplan_id = week_id;
  return describe_week(s, rs);
}

static void test_parse_week_text() {
  current_test = "parse week text";
  proto::DayPlan days[7];
  bool has[7];
  std::string err;
  CHECK(parse_week_text("Mo-Fr: 06:20 Home, 06:45 Away; Sa-Su: 00:00 Away, 08:30 Home, 09:00 Away", days, has, err));
  CHECK(has[0] && has[4] && has[5] && has[6]);
  CHECK(proto::describe_day_plan(days[0]) == "00:00 Away, 06:20 Home, 06:45 Away");  // 00:00 from the evening
  CHECK(proto::describe_day_plan(days[6]) == "00:00 Away, 08:30 Home, 09:00 Away");
  CHECK(parse_week_text("sat: 09:00 home, 07:00 AWAY, Preheat 08:30 30 min", days, has, err));
  CHECK(!has[0] && has[5] && proto::describe_day_plan(days[5]) == "00:00 Home, 07:00 Away, 09:00 Home, Preheat 08:30 30 min");
  CHECK(parse_week_text("Fr-Mo: 07:00 Home, 22:00 Away", days, has, err) && has[4] && has[6] && has[0] && !has[1]);
  CHECK(parse_week_text("Mo, We: 07:00 Home, 22:00 Away", days, has, err) && has[0] && !has[1] && has[2]);
  CHECK(!parse_week_text("Mo-Fr: 06:20 Home; Mo: 07:00 Home", days, has, err) && err == "Mo is given twice");
  CHECK(!parse_week_text("Xy: 06:20 Home", days, has, err));
  CHECK(!parse_week_text("Mo: 06:20 Hot", days, has, err) && err.rfind("can't read", 0) == 0);
  // Every mode the Schedule sensor prints is read; the app's limits then allow only Home and Away.
  CHECK(!parse_week_text("Mo: 06:00 Asleep, 07:00 Away", days, has, err) && err == "Mo: entries must be Home or Away");
  CHECK(!parse_week_text("Mo: 00:00 antifreeze", days, has, err) && err == "Mo: entries must be Home or Away");
  CHECK(!parse_week_text("Mo: 06:21 Home", days, has, err) && err.rfind("Mo: ", 0) == 0);  // 5-minute steps
  CHECK(!parse_week_text("Mo: Preheat 06:00 20 min, 07:00 Home", days, has, err));
  CHECK(!parse_week_text("", days, has, err) && err == "no days given");
}

static void test_name_for_days() {
  current_test = "plan names";
  CHECK(name_for_days(0x7F, {0x7F}) == "Every day");
  CHECK(name_for_days(0x1F, {0x1F, 0x60}) == "Weekdays");
  CHECK(name_for_days(0x60, {0x1F, 0x60}) == "Weekend");
  CHECK(name_for_days(0x01, {0x01, 0x7E}) == "Monday");
  CHECK(name_for_days(0x7E, {0x01, 0x7E}) == "Rest of the week");
  CHECK(name_for_days(0x07, {0x07, 0x78}) == "Monday-Wednesday");  // neither has a proper name
  CHECK(name_for_days(0x78, {0x07, 0x78}) == "Thursday-Sunday");
  CHECK(name_for_days(0x1E, {0x01, 0x1E, 0x60}) == "Tuesday-Friday");  // three groups: no "rest"
  CHECK(name_for_days(0x15, {0x15, 0x6A}) == "Mo, We, Fr");
}

static Schedule schedule_of(const FakeSchedule &fs) {
  Schedule s;
  for (auto &d : fs.days)
    s.day_plans.push_back(d.second);
  for (auto &w : fs.weeks)
    s.week_plans.push_back(w.second);
  return s;
}

static void test_plan_schedule_change() {
  current_test = "plan schedule change";
  FakeSchedule fs = default_fake_schedule();
  Schedule s = schedule_of(fs);
  std::vector<PlanOp> ops;
  std::string err;
  std::string same = "Mo-Fr: 06:00 Home, 07:00 Away; Sa-Su: 08:00 Home, 09:00 Away";
  // Same content: only the names change.
  CHECK(plan_schedule_change(s, 0, same, {}, ops, err));
  CHECK(ops.size() == 2 && ops[0].kind == PlanOp::SET_DAY && ops[0].day.name == "Weekdays" &&
        ops[1].day.name == "Weekend");
  // With good names already: nothing to do.
  fs.days[0].name = "Weekdays";
  fs.days[1].name = "Weekend";
  s = schedule_of(fs);
  CHECK(plan_schedule_change(s, 0, same, {}, ops, err) && ops.empty());
  CHECK(plan_schedule_change(s, 0, "Sa-Su: 08:00 Home, 09:00 Away", {}, ops, err) && ops.empty());  // other days kept
  // Monday different: one plan added, the rest renamed, the week points Monday to the new one.
  CHECK(plan_schedule_change(s, 0, "Mo: 05:30 Home, 07:00 Away", {}, ops, err));
  CHECK(ops.size() == 3);
  CHECK(ops.size() == 3 && ops[0].kind == PlanOp::ADD_DAY && ops[0].day.name == "Monday" && ops[0].slot == 0);
  CHECK(ops.size() == 3 && ops[1].kind == PlanOp::SET_DAY && ops[1].day.id == 0 && ops[1].day.name == "Tuesday-Friday");
  CHECK(ops.size() == 3 && ops[2].kind == PlanOp::SET_WEEK && ops[2].day_slot[0] == 0 && ops[2].day_slot[1] == -1 &&
        ops[2].week.day_ids[1] == 0 && ops[2].week.day_ids[5] == 1);
  // Every day the same: plan 0 rewritten (used most), plan 1 removed after the week stops using it.
  CHECK(plan_schedule_change(s, 0, "Mo-Su: 07:00 Home, 22:00 Away", {}, ops, err));
  CHECK(ops.size() == 3 && ops[0].kind == PlanOp::SET_DAY && ops[0].day.id == 0 && ops[0].day.name == "Every day");
  CHECK(ops.size() == 3 && ops[1].kind == PlanOp::SET_WEEK && ops[1].week.day_ids[6] == 0);
  CHECK(ops.size() == 3 && ops[2].kind == PlanOp::REMOVE_DAY && ops[2].id == 1);
  // A plan another week plan uses is neither rewritten nor removed.
  proto::WeekPlan other = fs.weeks[0];
  other.id = 1;
  fs.weeks[1] = other;
  s = schedule_of(fs);
  CHECK(plan_schedule_change(s, 0, "Mo-Su: 07:00 Home, 22:00 Away", {}, ops, err));
  CHECK(ops.size() == 2 && ops[0].kind == PlanOp::ADD_DAY && ops[1].kind == PlanOp::SET_WEEK);
  // At most 8 day plans; unused plans not named by the bridge are spares and stay.
  for (uint32_t id = 2; id < 8; id++) {
    fs.days[id] = fs.days[0];
    fs.days[id].id = id;
    fs.days[id].name = "Spare " + std::to_string(id);
  }
  s = schedule_of(fs);
  CHECK(!plan_schedule_change(s, 0, "Mo: 05:30 Home, 07:00 Away", {}, ops, err) && err == "the remote holds at most 8 day plans");
  CHECK(!plan_schedule_change(s, 4, "Mo: 05:30 Home", {}, ops, err) && err == "no week plan with id 4");
}

static void test_add_and_remove_day_plans() {
  current_test = "add and remove day plans";
  Harness h;
  FakeSchedule fs = default_fake_schedule();
  install_fake_schedule(h, fs);
  h.session.poll();
  h.run(10000);
  proto::DayPlan plan;
  plan.name = "Test";
  plan.items = {{0, 2}, {10 * 3600, 0}, {11 * 3600, 2}};
  for (int i = 0; i < 5; i++) {
    CHECK(h.session.add_day_plan(plan).empty());
    h.run(20000);
    CHECK(h.schedule_writes.size() == (size_t) i + 1 && h.schedule_writes.back().first == ScheduleWriteResult::OK);
  }
  CHECK(fs.days.size() == 7 && h.session.schedule().day_plans.size() == 7);
  CHECK(fs.days.count(6) && fs.days[6].name == "Test");
  CHECK(h.session.remove_day_plan(0) == "day plan 0 is in use by week plan 0");
  for (uint32_t id = 2; id <= 6; id++) {
    CHECK(h.session.remove_day_plan(id).empty());
    h.run(20000);
  }
  CHECK(fs.days.size() == 2 && fs.day_removes == 5);
  CHECK(h.schedule_writes.size() == 10 && h.schedule_writes.back().first == ScheduleWriteResult::OK);
  CHECK(week_text(h.session.schedule()) == "Mo-Fr: 00:00 Away, 06:00 Home, 07:00 Away; Sa-Su: 00:00 Away, 08:00 Home, 09:00 Away");
  // Full: the ninth is refused before anything is sent.
  for (uint32_t id = 2; id < 8; id++)
    fs.days[id] = plan, fs.days[id].id = id;
  h.session.request_schedule();
  h.session.poll();
  h.run(20000);
  CHECK(h.session.add_day_plan(plan) == "the remote holds at most 8 day plans");
}

static void test_set_schedule_round_trip() {
  current_test = "set schedule";
  Harness h;
  FakeSchedule fs = default_fake_schedule();
  install_fake_schedule(h, fs);
  h.session.poll();
  h.run(10000);
  std::string original = week_text(h.session.schedule());
  // Monday gets its own plan.
  h.clear();
  CHECK(h.session.set_schedule("Mo: 05:30 Home, 07:00 Away").empty());
  h.run(25000);
  CHECK(h.schedule_writes.size() == 1 && h.schedule_writes[0].first == ScheduleWriteResult::OK);
  auto ops = h.ops();
  CHECK(ops.size() >= 10 && (std::vector<uint32_t>(ops.begin(), ops.begin() + 10) ==
                             std::vector<uint32_t>{1, 177, 161, 161, 163, 169, 173, 173, 175, 1}));  // read, add,
                                                                                                      // renames, week

  CHECK(fs.days.size() == 3 && fs.days[2].name == "Monday" && fs.weeks[0].day_ids[0] == 2);
  CHECK(fs.days[0].name == "Tuesday-Friday" && fs.days[1].name == "Weekend");
  CHECK(week_text(h.session.schedule()) ==
        "Mo: 00:00 Away, 05:30 Home, 07:00 Away; Tu-Fr: 00:00 Away, 06:00 Home, 07:00 Away; Sa-Su: 00:00 Away, 08:00 Home, 09:00 Away");
  // Back: the Monday plan goes away again.
  CHECK(h.session.set_schedule(original).empty());
  h.run(25000);
  CHECK(h.schedule_writes.size() == 2 && h.schedule_writes[1].first == ScheduleWriteResult::OK);
  CHECK(fs.days.size() == 2 && fs.day_removes == 1 && fs.days[0].name == "Weekdays");
  CHECK(week_text(h.session.schedule()) == original);
  // Again: a read shows nothing left to do.
  int writes = fs.day_writes + fs.week_writes + fs.day_adds + fs.day_removes;
  CHECK(h.session.set_schedule(original).empty());
  h.run(10000);
  CHECK(h.schedule_writes.size() == 3 && h.schedule_writes[2].first == ScheduleWriteResult::UNCHANGED);
  CHECK(fs.day_writes + fs.week_writes + fs.day_adds + fs.day_removes == writes);
  CHECK(h.session.set_schedule("Mo: 06:21 Home") == "Mo: entry times must be 5-minute steps within the day");
}

static void test_set_schedule_failures() {
  current_test = "set schedule failures";
  {
    Harness h;  // the add is rejected: nothing after it is written, reported after the read-back
    FakeSchedule fs = default_fake_schedule();
    install_fake_schedule(h, fs);
    h.session.poll();
    h.run(10000);
    h.handlers[proto::OP_ADD_DAY_PLAN_REQUEST] = [](const Request &r) {
      return std::vector<Bytes>{frame(proto::OP_ADD_DAY_PLAN_CONFIRM, 14, r.ref)};
    };
    h.clear();
    CHECK(h.session.set_schedule("Mo: 05:30 Home, 07:00 Away").empty());
    h.run(25000);
    CHECK(fs.day_writes == 0 && fs.week_writes == 0);
    CHECK(h.schedule_writes.size() == 1 && h.schedule_writes[0].first == ScheduleWriteResult::FAILED);
    CHECK(h.schedule_writes.size() == 1 &&
          h.schedule_writes[0].second == "day plan add rejected (result 14); 3 later step(s) not written");
    CHECK(!h.session.plan_writes_pending());
  }
  {
    Harness h;  // old firmware: add stores an empty plan, the content follows with SetDayplan
    FakeSchedule fs = default_fake_schedule();
    fs.add_stores_empty = true;
    install_fake_schedule(h, fs);
    h.session.poll();
    h.run(10000);
    CHECK(h.session.set_schedule("Mo: 05:30 Home, 07:00 Away").empty());
    h.run(25000);
    CHECK(h.schedule_writes.size() == 1 && h.schedule_writes[0].first == ScheduleWriteResult::OK);
    CHECK(fs.days[2].name == "Monday" && fs.days[2].items.size() == 3);
  }
}

static Schedule one_week(const std::vector<proto::DayPlan> &plans, const uint32_t (&ids)[7]) {
  Schedule s;
  s.day_plans = plans;
  proto::WeekPlan week;
  week.name = "W";
  for (int d = 0; d < 7; d++) {
    week.has_day[d] = true;
    week.day_ids[d] = ids[d];
  }
  s.week_plans = {week};
  return s;
}

static proto::DayPlan day_plan(uint32_t id, const std::string &name, uint32_t home_h, uint32_t away_h) {
  return proto::DayPlan{id, name, {{0, 2}, {home_h * 3600, 0}, {away_h * 3600, 2}}, {}};
}

static void test_plan_order_is_safe() {
  current_test = "plan order";
  std::vector<PlanOp> ops;
  std::string err;
  // One plan for every day, new content for weekdays and weekend. Rewriting it first would give the weekend the
  // weekday content until the week write: the weekend moves to a new plan first, then the plan is rewritten.
  Schedule s = one_week({day_plan(0, "Every day", 6, 7)}, {0, 0, 0, 0, 0, 0, 0});
  CHECK(plan_schedule_change(s, 0, "Mo-Fr: 05:00 Home, 07:00 Away; Sa-Su: 08:00 Home, 10:00 Away", {}, ops, err));
  CHECK(ops.size() == 3 && ops[0].kind == PlanOp::ADD_DAY && ops[0].day.name == "Weekend");
  CHECK(ops.size() == 3 && ops[1].kind == PlanOp::SET_WEEK && ops[1].day_slot[5] == 0 && ops[1].day_slot[4] == -1);
  CHECK(ops.size() == 3 && ops[2].kind == PlanOp::SET_DAY && ops[2].day.id == 0 && ops[2].day.name == "Weekdays");
  // A plan whose days all get the new content is rewritten before the week write.
  s = one_week({day_plan(0, "Weekdays", 6, 7), day_plan(1, "Weekend", 8, 9)}, {0, 0, 0, 0, 0, 1, 1});
  CHECK(plan_schedule_change(s, 0, "Mo-Su: 05:00 Home, 07:00 Away", {}, ops, err));
  CHECK(ops.size() == 3 && ops[0].kind == PlanOp::SET_DAY && ops[0].day.id == 0);
  CHECK(ops.size() == 3 && ops[1].kind == PlanOp::SET_WEEK && ops[2].kind == PlanOp::REMOVE_DAY && ops[2].id == 1);
  // Overlapping both ways (Mo-We / Th-Su to Tu-Th / Fr-Mo): neither order is safe, both groups get new plans.
  s = one_week({day_plan(0, "Monday-Wednesday", 6, 7), day_plan(1, "Thursday-Sunday", 8, 9)}, {0, 0, 0, 1, 1, 1, 1});
  CHECK(plan_schedule_change(s, 0, "Tu-Th: 05:00 Home, 07:00 Away; Fr-Mo: 10:00 Home, 11:00 Away", {}, ops, err));
  CHECK(ops.size() == 5 && ops[0].kind == PlanOp::ADD_DAY && ops[1].kind == PlanOp::ADD_DAY &&
        ops[2].kind == PlanOp::SET_WEEK && ops[3].kind == PlanOp::REMOVE_DAY && ops[4].kind == PlanOp::REMOVE_DAY);
}

static void test_plan_unused_plans() {
  current_test = "unused plans";
  std::vector<PlanOp> ops;
  std::string err;
  // Unused plans: 'Monday' (an add whose confirm got lost) is reused, one without a name removed, the app's spare
  // kept. Removals of unused plans come first.
  proto::DayPlan monday = day_plan(2, "Monday", 5, 7), unnamed = day_plan(3, "", 1, 2), spare = day_plan(4, "Spare", 1, 2);
  Schedule s = one_week({day_plan(0, "Weekdays", 6, 7), day_plan(1, "Weekend", 8, 9), monday, unnamed, spare},
                        {0, 0, 0, 0, 0, 1, 1});
  CHECK(plan_schedule_change(s, 0, "Mo: 05:00 Home, 07:00 Away", {}, ops, err));
  CHECK(ops.size() == 3 && ops[0].kind == PlanOp::REMOVE_DAY && ops[0].id == 3);
  CHECK(ops.size() == 3 && ops[1].kind == PlanOp::SET_DAY && ops[1].day.id == 0 && ops[1].day.name == "Tuesday-Friday");
  CHECK(ops.size() == 3 && ops[2].kind == PlanOp::SET_WEEK && ops[2].week.day_ids[0] == 2 && ops[2].day_slot[0] == -1);
  // With nothing else to do they are still cleaned up; a plan an earlier round released goes too.
  CHECK(plan_schedule_change(s, 0, "Mo-Fr: 06:00 Home, 07:00 Away", {}, ops, err));
  CHECK(ops.size() == 2 && ops[0].id == 2 && ops[1].id == 3);
  CHECK(plan_schedule_change(s, 0, "Mo-Fr: 06:00 Home, 07:00 Away", {4}, ops, err) && ops.size() == 3);
  CHECK(generated_plan_name("Rest of the week") && generated_plan_name("Mo, We, Fr") &&
        generated_plan_name("Tuesday-Thursday") && generated_plan_name(""));
  CHECK(!generated_plan_name("Spare") && !generated_plan_name("WD") && !generated_plan_name("Monday-Tuesday "));
}

static void test_plan_target() {
  current_test = "plan target";
  FakeSchedule fs = default_fake_schedule();
  Schedule s = schedule_of(fs);
  std::vector<PlanOp> ops;
  std::string err;
  ScheduleTarget t;
  // Week plans are compared with their names too.
  t.kind = ScheduleTarget::WEEK_PLAN;
  t.week = fs.weeks[0];
  CHECK(plan_target(s, t, ops, err) && ops.empty());
  t.week.name = "Renamed";
  CHECK(plan_target(s, t, ops, err) && ops.size() == 1 && ops[0].kind == PlanOp::SET_WEEK);
  t.kind = ScheduleTarget::DAY_PLAN;
  t.day = fs.days[1];
  CHECK(plan_target(s, t, ops, err) && ops.empty());
  t.day.name = "Renamed";
  CHECK(plan_target(s, t, ops, err) && ops.size() == 1 && ops[0].kind == PlanOp::SET_DAY);
  // A removal is done once the plan is gone, refused while a week uses it.
  t.kind = ScheduleTarget::REMOVE_DAY;
  t.id = 9;
  CHECK(plan_target(s, t, ops, err) && ops.empty());
  t.id = 1;
  CHECK(!plan_target(s, t, ops, err) && err == "day plan 1 is in use by week plan 0");
  CHECK(describe_ops({PlanOp{PlanOp::REMOVE_DAY, {}, {}, 3}}) == "day plan 3 still there");
}

static bool run_until_written(Harness &h, uint32_t max_ms = 400000) {
  for (uint32_t t = 0; t < max_ms && (h.schedule_writes.empty() || h.session.active()); t += 1000)
    h.run(1000);
  return !h.schedule_writes.empty();
}

static void test_add_day_plan_interrupted() {
  current_test = "add interrupted";
  // The add may have happened: it is not sent again, the result says so.
  Harness h;
  FakeSchedule fs = default_fake_schedule();
  install_fake_schedule(h, fs);
  h.session.poll();
  h.run(10000);
  fs.fault_at = 0;
  fs.fault = Fault::LOST_CONFIRM;
  CHECK(h.session.add_day_plan(day_plan(0, "Test", 10, 11)).empty());
  CHECK(run_until_written(h));
  CHECK(fs.day_adds == 1 && fs.days.size() == 3);
  CHECK(h.schedule_writes.size() == 1 && h.schedule_writes[0].first == ScheduleWriteResult::FAILED &&
        h.schedule_writes[0].second == "interrupted; the plan may have been added, check the schedule");
  CHECK(!h.session.plan_writes_pending());
}

static void test_set_schedule_reads_first() {
  current_test = "set schedule reads first";
  // The app changes the weekend after the bridge's last read: the change is planned from a read in the write
  // session, so the new weekend stays and nothing is written for it.
  Harness h;
  FakeSchedule fs = default_fake_schedule();
  install_fake_schedule(h, fs);
  h.session.poll();
  h.run(10000);
  fs.days[1].items = {{0, 2}, {10 * 3600, 0}, {12 * 3600, 2}};
  CHECK(h.session.set_schedule("Mo-Fr: 06:30 Home, 07:00 Away").empty());
  CHECK(run_until_written(h) && h.schedule_writes[0].first == ScheduleWriteResult::OK);
  CHECK(fs.days.size() == 2 && proto::describe_day_plan(fs.days[1]) == "00:00 Away, 10:00 Home, 12:00 Away");
  CHECK(proto::describe_day_plan(fs.days[0]) == "00:00 Away, 06:30 Home, 07:00 Away");
}

// After a write the remote applied: no week plan points at a missing plan and (unless the app edits plans too)
// every day follows its content from before or after the change.
static bool consistent(const FakeSchedule &fs, const Schedule &before, const Schedule &after, bool check_content) {
  for (auto &w : fs.weeks)
    for (uint32_t id : w.second.day_ids)
      if (!fs.days.count(id))
        return false;
  if (!check_content)
    return true;
  const proto::WeekPlan &week = fs.weeks.at(0);
  for (int d = 0; d < 7; d++) {
    const proto::DayPlan &now = fs.days.at(week.day_ids[d]);
    if (!same_content(now, *before.day_plan(before.week_plan(0)->day_ids[d])) &&
        !same_content(now, *after.day_plan(after.week_plan(0)->day_ids[d])))
      return false;
  }
  return true;
}

// Fault injection against the schedule-writing bugs fixed earlier: each plan write of a change is hit by each fault in turn, then
// sessions run until the change reports. It must succeed and leave the remote as without faults: same week, no
// extra plans (an app's spare plan stays), never a week pointing at a missing plan or a day on content that is
// neither its old nor its new one.
static void test_schedule_change_faults() {
  struct Case {
    const char *text;
    bool all_days;  // every day given: an app edit of Monday's plan is overwritten
  };
  const Case cases[] = {
      {"Mo: 05:30 Home, 07:00 Away", false},                                     // add, renames, week
      {"Sa-Su: 07:00 Home, 22:00 Away", false},                                  // rewrite in place
      {"Mo-Su: 07:00 Home, 22:00 Away", true},                                   // rewrite, week, removal
      {"Mo-Su: 06:00 Home, 07:00 Away", true},                                   // keep one, week, removal
      {"Mo-Fr: 06:30 Home, 07:30 Away; Sa-Su: 07:00 Home, 10:00 Away", true},    // two rewrites
      {"Mo-Th: 06:00 Home, 07:00 Away; Fr-Su: 08:00 Home, 09:00 Away", true},    // renames and week only
      {"Mo-Th: 05:00 Home, 07:00 Away; Fr: 07:00 Home, 08:00 Away; Sa-Su: 08:00 Home, 09:00 Away", true},  // add,
                                                                                 // week, rewrite after the week
  };
  const Fault faults[] = {Fault::DROP_NOT_APPLIED, Fault::DROP_APPLIED, Fault::LOST_REQUEST,
                          Fault::LOST_CONFIRM,     Fault::LATE_CONFIRM, Fault::APP_EDIT};
  static std::string name;
  for (auto &c : cases) {
    name = std::string("faults, ") + c.text;
    current_test = name.c_str();
    // Without faults: the expected result and the number of writes.
    FakeSchedule base = default_fake_schedule();
    {
      Harness h;
      install_fake_schedule(h, base);
      h.session.poll();
      h.run(10000);
      CHECK(h.session.set_schedule(c.text).empty());
      CHECK(run_until_written(h) && h.schedule_writes[0].first == ScheduleWriteResult::OK);
    }
    Schedule before = schedule_of(default_fake_schedule()), after = schedule_of(base);
    for (int at = 0; at < base.plan_writes; at++) {
      for (Fault fault : faults) {
        if (fault == Fault::APP_EDIT && !c.all_days && at > 0)
          continue;
        name = std::string("faults, ") + c.text + ", write " + std::to_string(at) + ", fault " +
               std::to_string((int) fault);
        current_test = name.c_str();
        Harness h;
        FakeSchedule fs = default_fake_schedule();
        install_fake_schedule(h, fs);
        h.session.poll();
        h.run(10000);
        fs.fault_at = at;
        fs.fault = fault;
        fs.app_edit = [&c](FakeSchedule &f) {
          uint32_t id = 0;
          while (f.days.count(id))
            id++;
          f.days[id] = proto::DayPlan{id, "Spare", {{0, 2}}, {}};
          if (c.all_days)
            f.days[f.weeks[0].day_ids[0]].items = {{0, 2}, {12 * 3600, 0}, {13 * 3600, 2}};
        };
        bool ok = true;
        fs.on_change = [&] { ok &= consistent(fs, before, after, fault != Fault::APP_EDIT); };
        CHECK(h.session.set_schedule(c.text).empty());
        CHECK(run_until_written(h));
        CHECK(h.schedule_writes.size() == 1 && h.schedule_writes[0].first == ScheduleWriteResult::OK);
        CHECK(ok);
        CHECK(week_text(schedule_of(fs)) == week_text(after));
        CHECK(fs.days.size() == base.days.size() + (fault == Fault::APP_EDIT ? 1 : 0));
        CHECK(fs.day_adds == base.day_adds);  // an add that may have happened is never sent again
        CHECK(!h.session.plan_writes_pending());
        if (h.schedule_writes.size() == 1 && h.schedule_writes[0].first != ScheduleWriteResult::OK)
          std::printf("  -> %s\n", h.schedule_writes[0].second.c_str());
      }
    }
  }
}

static void test_session_gap() {
  current_test = "session gap";
  Harness h;
  h.session.config().session_gap_ms = SessionConfig{}.session_gap_ms;
  CHECK(h.session.config().session_gap_ms == 20000);
  h.session.poll();
  while (h.session_ends.empty())
    h.run(10);
  CHECK(h.connects_called == 1);
  h.session.poll();  // right after the previous session
  h.run(19000);
  CHECK(h.connects_called == 1);
  h.run(2000);
  CHECK(h.connects_called == 2 && h.session.status() != BridgeStatus::UNREACHABLE);
}

static void test_poll_during_session_is_covered() {
  current_test = "poll during session";
  Harness h;
  h.session.poll();
  h.run(500);  // connected, reading
  CHECK(h.session.active());
  h.session.poll();  // e.g. the periodic poll fires during a "Refresh now" session
  h.run(60000);
  CHECK(h.connects_called == 1 && h.session.status() == BridgeStatus::OK);
  // A write requested during a session still gets its own session.
  h.handlers[proto::OP_SET_DEVICE_FUNCTION_MODE_REQUEST] = [](const Request &r) {
    return std::vector<Bytes>{frame(proto::OP_SET_DEVICE_FUNCTION_MODE_CONFIRM, 0, r.ref)};
  };
  h.session.poll();
  h.run(500);
  h.session.start_timer(5);
  h.run(10000);
  CHECK(h.connects_called == 3 && h.saw(BridgeStatus::WRITTEN));
  // So does a schedule read asked for during a session.
  h.session.config().want_schedule = true;
  h.run(60000);
  int before = h.connects_called;
  h.session.poll();
  h.run(500);
  h.session.request_schedule();
  h.session.poll();
  h.run(20000);
  CHECK(h.connects_called == before + 2);
}

static void test_active_target_unknown_mode_skips_asleep() {
  current_test = "active target, mode unknown";
  Harness h;
  h.handlers[proto::OP_GET_RUN_STATE_REQUEST] = [](const Request &) { return std::vector<Bytes>{}; };  // no mode
  h.handlers[proto::OP_SET_ROOM_VALUE_REQUEST] = [](const Request &r) {
    return std::vector<Bytes>{frame(proto::OP_SET_ROOM_VALUE_CONFIRM, 0, r.ref)};
  };
  h.session.poll();
  h.run(15000);
  CHECK(h.session.temp_mode() == -1);
  h.clear();
  h.session.set_active_target(210);  // target 20.0 = Home = Asleep: only Home changes
  h.run(15000);
  CHECK((h.ops() == std::vector<uint32_t>{1, 240, 194, 205}));
  CHECK(h.requests.size() == 4 && (written_setpoints(h.requests[3]) == std::vector<int32_t>{210, 200, 180}));
}

/// Run until the session sends a request of type `op` (at most `limit` ms).
static bool run_until_sent(Harness &h, uint32_t op, uint32_t limit = 10000) {
  for (uint32_t t = 0; t < limit; t += 10) {
    for (auto &r : h.requests)
      if (r.op == op)
        return true;
    h.run(10);
  }
  return false;
}

// A thermostat target queued while a poll runs, before its run-state read: written in that session, to the mode
// active now. (Once the poll read the rooms before the run state and only planned no-op writes for it.)
static void test_active_target_during_poll() {
  current_test = "active target during poll";
  Harness h;
  h.temp_mode = 2;  // Away; the bridge hasn't read any run state yet
  h.handlers[proto::OP_SET_ROOM_VALUE_REQUEST] = [](const Request &r) {
    return std::vector<Bytes>{frame(proto::OP_SET_ROOM_VALUE_CONFIRM, 0, r.ref)};
  };
  h.session.poll();
  CHECK(run_until_sent(h, proto::OP_SET_ADDRESS_REQUEST));
  h.session.set_active_target(190);
  h.run(5000);
  auto ops = h.ops();
  CHECK(ops.size() >= 4 && (std::vector<uint32_t>(ops.end() - 3, ops.end()) == std::vector<uint32_t>{240, 194, 205}));
  CHECK(h.session.stats().connections == 1);
  if (!h.requests.empty() && h.requests.back().op == proto::OP_SET_ROOM_VALUE_REQUEST)
    CHECK((written_setpoints(h.requests.back()) == std::vector<int32_t>{200, 200, 190}));
}

static void test_command_queued_during_write_survives() {
  current_test = "command queued during write";
  // The remote applies setpoint writes at once but confirms them only after 2 s; `confirm` false: never.
  auto slow_setpoints = [](Harness &h, bool confirm) {
    h.handlers[proto::OP_SET_ROOM_VALUE_REQUEST] = [&h, confirm](const Request &r) {
      auto sp = written_setpoints(r);
      for (int m = 0; m < 3; m++)
        h.room.setpoints[m] = sp[m];
      if (confirm)
        h.reply_later(frame(proto::OP_SET_ROOM_VALUE_CONFIRM, 0, r.ref), 2000);
      return std::vector<Bytes>{};
    };
  };
  auto setpoint_writes = [](const Harness &h) {
    std::vector<std::vector<int32_t>> out;
    for (auto &r : h.requests)
      if (r.op == proto::OP_SET_ROOM_VALUE_REQUEST)
        out.push_back(written_setpoints(r));
    return out;
  };
  {
    Harness h;  // same mode: the newer value is written next
    h.session.poll();
    h.run(5000);
    slow_setpoints(h, true);
    h.clear();
    h.session.set_setpoint(0, 210);
    CHECK(run_until_sent(h, proto::OP_SET_ROOM_VALUE_REQUEST));
    h.session.set_setpoint(0, 220);
    h.run(3000);
    CHECK(h.session.pending_setpoint(0) == 220);
    h.run(40000);
    CHECK((setpoint_writes(h) == std::vector<std::vector<int32_t>>{{210, 200, 180}, {220, 200, 180}}));
    CHECK(h.session.pending_setpoint(0) == -1 && h.room.setpoints[0] == 220);
  }
  {
    Harness h;  // another mode: written next, merged with the first write's value
    h.session.poll();
    h.run(5000);
    slow_setpoints(h, true);
    h.clear();
    h.session.set_setpoint(0, 210);
    CHECK(run_until_sent(h, proto::OP_SET_ROOM_VALUE_REQUEST));
    h.session.set_setpoint(2, 170);
    h.run(3000);
    CHECK(h.session.pending_setpoint(0) == -1 && h.session.pending_setpoint(2) == 170);
    h.run(40000);
    CHECK((setpoint_writes(h) == std::vector<std::vector<int32_t>>{{210, 200, 180}, {210, 200, 170}}));
  }
  {
    Harness h;  // first write unconfirmed (timeout): the newer value still survives
    h.session.poll();
    h.run(5000);
    slow_setpoints(h, false);
    h.clear();
    h.session.set_setpoint(0, 210);
    CHECK(run_until_sent(h, proto::OP_SET_ROOM_VALUE_REQUEST));
    h.session.set_setpoint(0, 220);
    h.run(21000);
    CHECK(h.session.pending_setpoint(0) == 220);
    h.run(60000);
    auto writes = setpoint_writes(h);
    CHECK(writes.size() >= 2 && writes.back()[0] == 220 && h.room.setpoints[0] == 220);
  }
  {
    Harness h;  // link drops before the confirm: the latest value is written next
    h.session.poll();
    h.run(5000);
    h.handlers[proto::OP_SET_ROOM_VALUE_REQUEST] = [&h](const Request &) {
      h.disconnect();
      return std::vector<Bytes>{};
    };
    h.clear();
    h.session.set_setpoint(0, 210);
    CHECK(run_until_sent(h, proto::OP_SET_ROOM_VALUE_REQUEST));
    h.session.set_setpoint(0, 220);
    h.run(1000);
    CHECK(!h.session.active() && h.session.pending_setpoint(0) == 220);
    slow_setpoints(h, true);
    h.clear();
    h.run(40000);
    CHECK((setpoint_writes(h) == std::vector<std::vector<int32_t>>{{220, 200, 180}}));
  }
  {
    Harness h;  // timer: the countdown follows the sent duration, the newer one is written next
    h.session.poll();
    h.run(5000);
    std::vector<uint32_t> durations;
    h.handlers[proto::OP_SET_DEVICE_FUNCTION_MODE_REQUEST] = [&h, &durations](const Request &r) {
      std::vector<proto::Field> fields;
      proto::decode_message(r.body.data(), r.body.size(), fields);
      for (auto &f : fields)
        if (f.number == 4)
          durations.push_back(f.value);
      h.reply_later(frame(proto::OP_SET_DEVICE_FUNCTION_MODE_CONFIRM, 0, r.ref), 2000);
      return std::vector<Bytes>{};
    };
    h.clear();
    h.session.start_timer(15);
    CHECK(run_until_sent(h, proto::OP_SET_DEVICE_FUNCTION_MODE_REQUEST));
    h.session.start_timer(45);
    h.run(3000);
    uint32_t left = h.session.timer_remaining_s();
    CHECK(left > 14 * 60 && left <= 15 * 60);
    h.run(40000);
    CHECK((durations == std::vector<uint32_t>{15, 45}));
    left = h.session.timer_remaining_s();
    CHECK(left > 44 * 60 && left <= 45 * 60);
  }
  {
    Harness h;  // run mode
    h.session.poll();
    h.run(5000);
    std::vector<uint8_t> modes;
    h.handlers[proto::OP_SET_RUN_STATE_REQUEST] = [&h, &modes](const Request &r) {
      modes.push_back(r.body.at(1));
      h.reply_later(frame(proto::OP_SET_RUN_STATE_CONFIRM, 0, r.ref, r.body), 2000);
      return std::vector<Bytes>{};
    };
    h.clear();
    h.session.set_run_mode(RunModeChoice::MANUAL);
    CHECK(run_until_sent(h, proto::OP_SET_RUN_STATE_REQUEST));
    h.session.set_run_mode(RunModeChoice::AUTOMATIC);
    h.run(3000);
    CHECK(h.session.pending_run_mode() == static_cast<int>(RunModeChoice::AUTOMATIC));
    h.run(40000);
    CHECK((modes == std::vector<uint8_t>{proto::RUN_MODE_MANUAL, proto::RUN_MODE_AUTOMATIC}));
    CHECK(h.session.pending_run_mode() == -1);
  }
}

static void test_setpoint_write_reads_current_values() {
  current_test = "setpoint write reads first";
  Harness h;
  h.session.poll();
  h.run(5000);
  h.room.setpoints[2] = 170;  // Away changed in the app after the poll
  h.handlers[proto::OP_SET_ROOM_VALUE_REQUEST] = [](const Request &r) {
    return std::vector<Bytes>{frame(proto::OP_SET_ROOM_VALUE_CONFIRM, 0, r.ref)};
  };
  h.clear();
  h.session.set_setpoint(0, 210);
  h.run(3000);
  CHECK((h.ops() == std::vector<uint32_t>{1, 240, 194, 205}));
  CHECK((written_setpoints(h.requests[3]) == std::vector<int32_t>{210, 200, 170}));
}

int main() {
  test_poll_happy_path();
  test_boot_delay();
  test_reply_needs_poke();
  test_unreachable_retries();
  test_failed_connect_is_quiet_when_retry_works();
  test_setpoint_write_via_rooms_update();
  test_unrelated_rooms_update_ignored();
  test_write_postponed_until_rooms_known();
  test_timer_accepted_then_reported();
  test_timer_replaced();
  test_timer_rejected();
  test_timer_stopped_on_remote();
  test_disconnect_mid_session_retries();
  test_pairing();
  test_not_paired();
  test_untested_firmware();
  test_battery_low_from_run_state();
  test_millis_wrap();
  test_active_target_follows_mode();
  test_active_target_after_mode_switch();
  test_session_stats();
  test_configured_retries_and_timeout();
  test_configured_setpoint_range();
  test_real_replay();
  test_readback_only_without_room_data();
  test_readback_retried_when_remote_busy();
  test_per_mode_setpoint_limits();
  test_overheat_alarm();
  test_active_target_unknown_mode_skips_asleep();
  test_schedule_read();
  test_schedule_read_failure_keeps_previous();
  test_run_mode_write();
  test_run_mode_confirmed_by_indication();
  test_reply_framing_kept_across_requests();
  test_run_mode_written_before_timer();
  test_clock_check_and_sync();
  test_schedule_presentation();
  test_clock_without_get_time();
  test_next_switch_skips_unchanged_modes();
  test_day_plan_write();
  test_day_plan_write_checks();
  test_plan_write_not_applied();
  test_week_and_day_plan_in_one_session();
  test_session_gap();
  test_poll_during_session_is_covered();
  test_parse_week_text();
  test_name_for_days();
  test_plan_schedule_change();
  test_add_and_remove_day_plans();
  test_set_schedule_round_trip();
  test_set_schedule_failures();
  test_plan_order_is_safe();
  test_plan_unused_plans();
  test_plan_target();
  test_add_day_plan_interrupted();
  test_set_schedule_reads_first();
  test_schedule_change_faults();
  test_active_target_during_poll();
  test_command_queued_during_write_survives();
  test_setpoint_write_reads_current_values();
  if (failures == 0)
    std::printf("all session tests passed\n");
  return failures == 0 ? 0 : 1;
}
