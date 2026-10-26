#pragma once

// The remote's schedule (day plans and week plans): the model, how it is shown, and turning a text like
// "Mo-Fr: 06:20 Home, 06:45 Away; Sa-Su: 08:30 Home, 09:00 Away" into the plan changes that make the remote follow it.
// Plain C++ without ESPHome.

#include <cstdint>
#include <string>
#include <vector>

#include "wivar_proto.h"

namespace esphome::zehnder_wivar {

struct Schedule {
  std::vector<proto::DayPlan> day_plans;
  std::vector<proto::WeekPlan> week_plans;
  const proto::DayPlan *day_plan(uint32_t id) const {
    for (auto &p : this->day_plans)
      if (p.id == id)
        return &p;
    return nullptr;
  }
  const proto::WeekPlan *week_plan(uint32_t id) const {
    for (auto &p : this->week_plans)
      if (p.id == id)
        return &p;
    return nullptr;
  }
};

/// The week plan in use: cRunstate.todaysWeekplanId, else the only one. Null if unknown.
const proto::WeekPlan *active_week(const Schedule &schedule, const proto::RunState &rs);
/// The day plan the remote follows today: cRunstate.todaysDayplanId, else the week plan's entry for the weekday
/// (0 = Monday). Null if unknown.
const proto::DayPlan *today_plan(const Schedule &schedule, const proto::RunState &rs, int weekday);
/// "Mo-Fr: 07:00 Home, 22:00 Away; Sa-Su: 09:00 Home, 23:00 Away" for the week plan in use.
std::string describe_week(const Schedule &schedule, const proto::RunState &rs);
/// The next switch that changes the temperature mode: "22:00 Away", "Tomorrow 07:00 Home", or why there is none
/// (not in Automatic, no schedule).
std::string describe_next_switch(const Schedule &schedule, const proto::RunState &rs, int weekday, uint32_t now_s);

// ── Changing the schedule ────────────────────────────────────────────────────

/// The app's limits for the Wivar.
static const size_t MAX_DAY_PLANS = 8;
static const size_t MAX_WEEK_PLANS = 3;
/// Id sent with Add requests; the remote assigns the real one (the app uses 255 too).
static const uint32_t NEW_PLAN_ID = 255;

/// One change to the remote's plans. ADD_DAY stores the id the remote assigns in `slot`; in SET_WEEK, days with
/// day_slot[d] >= 0 take their id from that slot.
struct PlanOp {
  enum Kind : uint8_t { SET_DAY, ADD_DAY, REMOVE_DAY, SET_WEEK, ADD_WEEK, REMOVE_WEEK };
  Kind kind{SET_DAY};
  proto::DayPlan day;
  proto::WeekPlan week;
  uint32_t id{0};  // REMOVE_DAY / REMOVE_WEEK
  int slot{-1};
  int day_slot[7]{-1, -1, -1, -1, -1, -1, -1};
};

/// Same entries and timers (ids and names aside).
bool same_content(const proto::DayPlan &a, const proto::DayPlan &b);

/// Parse "Mo-Fr: 06:20 Home, 06:45 Away; Sa-Su: 00:00 Away, 08:30 Home; Mo: Preheat 06:00 30 min, ..." (the format
/// of the Schedule sensor). Days: Mo Tu We Th Fr Sa Su (or longer English names), ranges "Mo-Fr", lists "Mo, We".
/// Entries are sorted; a day that doesn't start at 00:00 gets "00:00 <mode of its last entry>" (the mode carried
/// over from the evening before). Days not mentioned have has_day[d] false. Modes are read as the sensor prints them
/// (Home, Asleep, Away, Antifreeze); each day is then validated like the app, which allows only Home and Away.
bool parse_week_text(const std::string &text, proto::DayPlan days[7], bool has_day[7], std::string &error);

/// Plan name for a group of days (bit 0 = Monday), given all groups of the week: "Every day", "Weekdays", "Weekend",
/// "Monday", "Rest of the week" (the bigger one of two when the other has such a name), "Tuesday-Thursday",
/// "Mo, We, Fr".
std::string name_for_days(uint8_t mask, const std::vector<uint8_t> &all);

/// True for names the bridge gives day plans (name_for_days) and for an empty name: an unused plan named like that
/// is one the bridge made (e.g. an add whose confirm got lost) and may be reused or removed. Other unused plans may be
/// spares kept in the app and are never touched.
bool generated_plan_name(const std::string &name);

/// The changes that make week plan `week_id` follow `text` (days not mentioned keep their plan). Day plans are kept
/// where the content matches, rewritten where that is safe, added otherwise, and removed when no week plan uses them
/// any more; unused plans the bridge made, and those in `released` (plans an earlier round of this change stopped
/// using), are reused or removed. Each day plan is named after its days. Every prefix of the steps leaves each day on
/// its old or its new content and no week pointing at a missing plan, so a change may stop after any step: unused
/// plans removed, adds, rewrites of plans whose current days all get the new content, the week, rewrites of plans
/// whose new days all used them already, removals. Empty `ops` means nothing to change. False with `error` when it
/// can't be done.
bool plan_schedule_change(const Schedule &schedule, uint32_t week_id, const std::string &text,
                          const std::vector<uint32_t> &released, std::vector<PlanOp> &ops, std::string &error);

/// What a schedule change should achieve. The session keeps it until a schedule read shows it done (or it fails) and
/// plans the steps from a read at the start of every write session, so an interrupted change continues from what the
/// remote actually holds instead of replaying steps.
struct ScheduleTarget {
  enum Kind : uint8_t { WEEK_TEXT, DAY_PLAN, WEEK_PLAN, REMOVE_DAY, REMOVE_WEEK, ADD_DAY, ADD_WEEK };
  Kind kind{WEEK_TEXT};
  std::string text;               // WEEK_TEXT
  uint32_t id{0};                 // WEEK_TEXT: the week plan; REMOVE_*: the plan to remove
  proto::DayPlan day;             // DAY_PLAN, ADD_DAY
  proto::WeekPlan week;           // WEEK_PLAN, ADD_WEEK
  std::vector<uint32_t> released;  // WEEK_TEXT: day plans earlier rounds meant to remove
};

/// The steps that make `schedule` match `target`; empty when it does. An ADD_* target always gives its add (once the
/// add is confirmed, the session turns it into DAY_PLAN / WEEK_PLAN with the assigned id).
bool plan_target(const Schedule &schedule, const ScheduleTarget &target, std::vector<PlanOp> &ops, std::string &error);
/// "day plan 2, day plan 5 still there, week plan 0": what a list of steps would still change.
std::string describe_ops(const std::vector<PlanOp> &ops);

}  // namespace esphome::zehnder_wivar
