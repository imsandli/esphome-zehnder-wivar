#include "wivar_schedule.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>

namespace esphome::zehnder_wivar {

// ── Schedule presentation ────────────────────────────────────────────────────

static const char *const DAY_SHORT[7] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
static const char *const DAY_LONG[7] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};

const proto::WeekPlan *active_week(const Schedule &schedule, const proto::RunState &rs) {
  if (rs.has_today_weekplan)
    if (auto *w = schedule.week_plan(rs.today_weekplan_id))
      return w;
  return schedule.week_plans.size() == 1 ? &schedule.week_plans[0] : nullptr;
}

const proto::DayPlan *today_plan(const Schedule &schedule, const proto::RunState &rs, int weekday) {
  if (rs.has_today_dayplan)
    if (auto *p = schedule.day_plan(rs.today_dayplan_id))
      return p;
  auto *week = active_week(schedule, rs);
  if (week == nullptr || weekday < 0 || weekday > 6 || !week->has_day[weekday])
    return nullptr;
  return schedule.day_plan(week->day_ids[weekday]);
}

std::string describe_week(const Schedule &schedule, const proto::RunState &rs) {
  auto *week = active_week(schedule, rs);
  if (week == nullptr)
    return "-";
  std::string out;
  for (int day = 0; day < 7;) {
    int last = day;
    while (last + 1 < 7 && week->has_day[last + 1] == week->has_day[day] &&
           week->day_ids[last + 1] == week->day_ids[day])
      last++;
    const proto::DayPlan *plan = week->has_day[day] ? schedule.day_plan(week->day_ids[day]) : nullptr;
    if (!out.empty())
      out += "; ";
    out += DAY_SHORT[day];
    if (last != day)
      out += std::string("-") + DAY_SHORT[last];
    out += ": " + (plan != nullptr ? proto::describe_day_plan(*plan) : std::string("?"));
    day = last + 1;
  }
  return out;
}

static std::string switch_text(const proto::DayPlanItem &item, const char *prefix) {
  char buf[32];
  snprintf(buf, sizeof buf, "%s%02u:%02u %s", prefix, (unsigned) (item.start_s / 3600 % 24),
           (unsigned) (item.start_s / 60 % 60), proto::short_temp_mode_name(item.temp_mode));
  return buf;
}

std::string describe_next_switch(const Schedule &schedule, const proto::RunState &rs, int weekday, uint32_t now_s) {
  if (rs.run_mode != proto::RUN_MODE_AUTOMATIC)
    return std::string("None (") + proto::run_mode_name(rs.run_mode) + ")";
  // Skip entries that don't change the mode (e.g. "00:00 Away" after "22:00 Away").
  const proto::DayPlan *today = today_plan(schedule, rs, weekday);
  int current = rs.temp_mode <= 3 ? (int) rs.temp_mode : -1;
  if (today != nullptr) {
    for (auto &item : today->items) {
      if (item.start_s <= now_s)
        current = item.temp_mode;
      else if ((int) item.temp_mode != current)
        return switch_text(item, "");
    }
  }
  // Tomorrow, from the week plan (today's plan id says nothing about tomorrow).
  proto::RunState tomorrow_rs = rs;
  tomorrow_rs.has_today_dayplan = false;
  if (const proto::DayPlan *tomorrow = today_plan(schedule, tomorrow_rs, (weekday + 1) % 7)) {
    for (auto &item : tomorrow->items) {
      if ((int) item.temp_mode != current)
        return switch_text(item, "Tomorrow ");
    }
  }
  return "-";
}

// ── Changing the schedule ────────────────────────────────────────────────────

bool same_content(const proto::DayPlan &a, const proto::DayPlan &b) {
  proto::DayPlan x = a, y = b;
  x.id = y.id = 0;
  return proto::same_plan(x, y);
}

static std::string trim(const std::string &s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos)
    return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

static std::vector<std::string> split(const std::string &s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    size_t pos = s.find(sep, start);
    out.push_back(trim(s.substr(start, pos == std::string::npos ? std::string::npos : pos - start)));
    if (pos == std::string::npos)
      return out;
    start = pos + 1;
  }
}

static std::string lower(std::string s) {
  for (auto &ch : s)
    ch = (char) std::tolower((unsigned char) ch);
  return s;
}

// "Mo", "mon", "Monday": at least the two letters of DAY_SHORT, then the long name's letters.
static int parse_day(const std::string &token) {
  std::string t = lower(trim(token));
  if (t.size() < 2)
    return -1;
  for (int d = 0; d < 7; d++)
    if (lower(DAY_LONG[d]).compare(0, t.size(), t) == 0)
      return d;
  return -1;
}

// "Mo-Fr", "Sa", "Mo, We", "Sa-Mo" (wraps).
static bool parse_days(const std::string &spec, uint8_t &mask) {
  mask = 0;
  for (auto &part : split(spec, ',')) {
    size_t dash = part.find('-');
    if (dash == std::string::npos) {
      int d = parse_day(part);
      if (d < 0)
        return false;
      mask |= 1 << d;
      continue;
    }
    int from = parse_day(part.substr(0, dash)), to = parse_day(part.substr(dash + 1));
    if (from < 0 || to < 0)
      return false;
    for (int d = from;; d = (d + 1) % 7) {
      mask |= 1 << d;
      if (d == to)
        break;
    }
  }
  return mask != 0;
}

static bool parse_time(const std::string &s, uint32_t &seconds) {
  unsigned h, m;
  char extra;
  if (std::sscanf(s.c_str(), "%u:%u%c", &h, &m, &extra) != 2 || h > 23 || m > 59)
    return false;
  seconds = h * 3600 + m * 60;
  return true;
}

// "06:20 Home" / "06:45 Away" / "Preheat 06:30 90 min". Every mode describe_day_plan prints is read back; which
// ones a plan may use is up to validate_day_plan.
static bool parse_entry(const std::string &entry, proto::DayPlan &plan, std::string &error) {
  std::vector<std::string> words;
  for (auto &w : split(entry, ' '))
    if (!w.empty())
      words.push_back(w);
  if (words.size() == 2) {
    proto::DayPlanItem item;
    std::string mode = lower(words[1]);
    int temp_mode = -1;
    for (uint32_t m = 0; m < 4; m++)
      if (mode == lower(proto::short_temp_mode_name(m)))
        temp_mode = (int) m;
    if (!parse_time(words[0], item.start_s) || temp_mode < 0) {
      error = "can't read \"" + entry + "\" (expected e.g. \"06:20 Home\")";
      return false;
    }
    item.temp_mode = temp_mode;
    plan.items.push_back(item);
    return true;
  }
  if ((words.size() == 3 || words.size() == 4) && lower(words[0]) == "preheat" &&
      (words.size() == 3 || lower(words[3]) == "min")) {
    proto::FunctionPlanItem fn;
    unsigned minutes;
    char extra;
    if (parse_time(words[1], fn.start_s) && std::sscanf(words[2].c_str(), "%u%c", &minutes, &extra) == 1) {
      fn.mode = 0;
      fn.duration_min = minutes;
      fn.has_device = true;  // the Wivar's only device; the app sets it for towel preheat
      fn.device_id = 0;
      plan.functions.push_back(fn);
      return true;
    }
  }
  error = "can't read \"" + entry + "\" (expected e.g. \"06:20 Home\" or \"Preheat 06:00 30 min\")";
  return false;
}

bool parse_week_text(const std::string &text, proto::DayPlan days[7], bool has_day[7], std::string &error) {
  for (int d = 0; d < 7; d++) {
    has_day[d] = false;
    days[d] = proto::DayPlan{};
  }
  bool any = false;
  for (auto &segment : split(text, ';')) {
    if (segment.empty())
      continue;
    size_t colon = segment.find(':');
    uint8_t mask;
    if (colon == std::string::npos || !parse_days(segment.substr(0, colon), mask)) {
      error = "can't read the days in \"" + segment + "\" (expected e.g. \"Mo-Fr: ...\")";
      return false;
    }
    proto::DayPlan plan;
    for (auto &entry : split(segment.substr(colon + 1), ','))
      if (!entry.empty() && !parse_entry(entry, plan, error))
        return false;
    std::sort(plan.items.begin(), plan.items.end(), [](auto &a, auto &b) { return a.start_s < b.start_s; });
    std::sort(plan.functions.begin(), plan.functions.end(), [](auto &a, auto &b) { return a.start_s < b.start_s; });
    if (!plan.items.empty() && plan.items.front().start_s != 0)
      plan.items.insert(plan.items.begin(), proto::DayPlanItem{0, plan.items.back().temp_mode});
    for (int d = 0; d < 7; d++) {
      if (!(mask & (1 << d)))
        continue;
      if (has_day[d]) {
        error = std::string(DAY_SHORT[d]) + " is given twice";
        return false;
      }
      std::string why = proto::validate_day_plan(plan);
      if (!why.empty()) {
        error = std::string(DAY_SHORT[d]) + ": " + why;
        return false;
      }
      days[d] = plan;
      has_day[d] = true;
      any = true;
    }
  }
  if (!any) {
    error = "no days given";
    return false;
  }
  return true;
}

static bool proper_name(uint8_t mask) {
  mask &= 0x7F;
  return mask == 0x7F || mask == 0x1F || mask == 0x60 || (mask != 0 && (mask & (mask - 1)) == 0);
}

std::string name_for_days(uint8_t mask, const std::vector<uint8_t> &all) {
  mask &= 0x7F;
  int count = 0, first = -1;
  for (int d = 0; d < 7; d++)
    if (mask & (1 << d)) {
      count++;
      if (first < 0)
        first = d;
    }
  if (mask == 0x7F)
    return "Every day";
  if (mask == 0x1F)
    return "Weekdays";
  if (mask == 0x60)
    return "Weekend";
  if (count == 1)
    return DAY_LONG[first];
  if (all.size() == 2 && count >= 3 && proper_name(all[0] == mask ? all[1] : all[0]))
    return "Rest of the week";
  bool contiguous = true;
  for (int d = first; d < first + count; d++)
    contiguous &= d < 7 && (mask & (1 << d));
  if (contiguous)
    return std::string(DAY_LONG[first]) + "-" + DAY_LONG[first + count - 1];
  std::string out;
  for (int d = 0; d < 7; d++)
    if (mask & (1 << d))
      out += (out.empty() ? "" : ", ") + std::string(DAY_SHORT[d]);
  return out;
}

bool generated_plan_name(const std::string &name) {
  if (name.empty() || name == "Rest of the week")
    return true;
  for (int mask = 1; mask < 0x80; mask++)
    if (name_for_days(mask, {(uint8_t) mask}) == name)
      return true;
  return false;
}

static int count_days(uint8_t mask) {
  int n = 0;
  for (int d = 0; d < 7; d++)
    n += (mask >> d) & 1;
  return n;
}

static bool contains(const std::vector<uint32_t> &ids, uint32_t id) {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

bool plan_schedule_change(const Schedule &schedule, uint32_t week_id, const std::string &text,
                          const std::vector<uint32_t> &released, std::vector<PlanOp> &ops, std::string &error) {
  ops.clear();
  const proto::WeekPlan *week = schedule.week_plan(week_id);
  if (week == nullptr) {
    error = "no week plan with id " + std::to_string(week_id);
    return false;
  }
  proto::DayPlan parsed[7];
  bool has_day[7];
  if (!parse_week_text(text, parsed, has_day, error))
    return false;

  // Target content per weekday: as given, else the day's current plan.
  proto::DayPlan target[7];
  for (int d = 0; d < 7; d++) {
    if (has_day[d]) {
      target[d] = parsed[d];
      continue;
    }
    const proto::DayPlan *cur = week->has_day[d] ? schedule.day_plan(week->day_ids[d]) : nullptr;
    if (cur == nullptr) {
      error = "the current plan for a day not given is unknown";
      return false;
    }
    target[d] = *cur;
  }

  // Days with the same content share a day plan.
  struct Group {
    enum Use : uint8_t { NONE, KEEP, BEFORE, AFTER, ADD };
    uint8_t mask{0};
    proto::DayPlan content;
    Use use{NONE};
    uint32_t id{0};
    int slot{-1};
  };
  std::vector<Group> groups;
  for (int d = 0; d < 7; d++) {
    auto it = std::find_if(groups.begin(), groups.end(), [&](auto &g) { return same_content(g.content, target[d]); });
    if (it == groups.end()) {
      groups.push_back(Group{});
      it = groups.end() - 1;
      it->content = target[d];
    }
    it->mask |= 1 << d;
  }
  std::vector<uint8_t> masks;
  for (auto &g : groups)
    masks.push_back(g.mask);
  for (auto &g : groups)
    g.content.name = name_for_days(g.mask, masks);

  // The days of this week that use a plan now, and whether another week plan uses it (then it is never rewritten
  // or removed).
  auto users = [&](uint32_t id) {
    uint8_t mask = 0;
    for (int d = 0; d < 7; d++)
      if (week->has_day[d] && week->day_ids[d] == id)
        mask |= 1 << d;
    return mask;
  };
  auto shared = [&](uint32_t id) {
    for (auto &w : schedule.week_plans)
      if (w.id != week_id)
        for (int d = 0; d < 7; d++)
          if (w.has_day[d] && w.day_ids[d] == id)
            return true;
    return false;
  };
  // Plans this change may use: those this week uses (weekday order), then unused ones the bridge made or an earlier
  // round released. Other unused plans may be spares kept in the app.
  std::vector<uint32_t> candidates;
  for (int d = 0; d < 7; d++)
    if (week->has_day[d] && schedule.day_plan(week->day_ids[d]) != nullptr && !contains(candidates, week->day_ids[d]))
      candidates.push_back(week->day_ids[d]);
  for (auto &p : schedule.day_plans)
    if (users(p.id) == 0 && !shared(p.id) && (generated_plan_name(p.name) || contains(released, p.id)))
      candidates.push_back(p.id);

  // The candidate with most of the group's days. With the same content it is kept. Otherwise it is rewritten where
  // that never shows a day wrong content: before the week write if all days using it now get the new content, after
  // it if all the group's days use it already (the others move away with the week write). Else the group gets a
  // new plan.
  std::vector<uint32_t> taken;
  auto take = [&](Group &g, bool same) {
    int best = -1, best_days = -1;
    auto best_use = Group::NONE;
    for (size_t i = 0; i < candidates.size(); i++) {
      uint32_t id = candidates[i];
      if (contains(taken, id))
        continue;
      uint8_t now = users(id);
      auto use = Group::NONE;
      if (same) {
        if (same_content(*schedule.day_plan(id), g.content))
          use = Group::KEEP;
      } else if (!shared(id)) {
        if ((now & ~g.mask) == 0)
          use = Group::BEFORE;
        else if ((g.mask & ~now) == 0)
          use = Group::AFTER;
      }
      int days = count_days(now & g.mask);
      if (use != Group::NONE && days > best_days) {
        best = (int) i;
        best_days = days;
        best_use = use;
      }
    }
    if (best < 0)
      return false;
    g.use = best_use;
    g.id = candidates[best];
    taken.push_back(g.id);
    return true;
  };
  auto set_op = [](const Group &g) {
    PlanOp op;
    op.kind = PlanOp::SET_DAY;
    op.day = g.content;
    op.day.id = g.id;
    return op;
  };

  std::vector<PlanOp> unused_removes, adds, before, after, removes;
  // 1. Plans that already have the content: keep (rename unless other weeks use them too).
  for (auto &g : groups)
    if (take(g, true) && schedule.day_plan(g.id)->name != g.content.name && !shared(g.id))
      before.push_back(set_op(g));
  // 2. Rewrite where safe; 3. add the rest.
  int slots = 0;
  for (auto &g : groups) {
    if (g.use != Group::NONE)
      continue;
    if (take(g, false)) {
      (g.use == Group::BEFORE ? before : after).push_back(set_op(g));
      continue;
    }
    PlanOp op;
    op.kind = PlanOp::ADD_DAY;
    op.day = g.content;
    op.day.id = NEW_PLAN_ID;
    op.slot = g.slot = slots++;
    g.use = Group::ADD;
    adds.push_back(op);
  }
  // Candidates left over: unused ones go first (frees slots), those this week used once the week no longer does.
  for (uint32_t id : candidates) {
    if (contains(taken, id) || shared(id))
      continue;
    PlanOp op;
    op.kind = PlanOp::REMOVE_DAY;
    op.id = id;
    (users(id) == 0 ? unused_removes : removes).push_back(op);
  }
  if (schedule.day_plans.size() - unused_removes.size() + adds.size() > MAX_DAY_PLANS) {
    error = "the remote holds at most 8 day plans";
    return false;
  }

  // The week plan.
  PlanOp week_op;
  week_op.kind = PlanOp::SET_WEEK;
  week_op.week = *week;
  bool week_changes = false;
  for (auto &g : groups) {
    for (int d = 0; d < 7; d++) {
      if (!(g.mask & (1 << d)))
        continue;
      if (g.slot >= 0) {
        week_op.day_slot[d] = g.slot;
        week_changes = true;
      } else if (!week->has_day[d] || week->day_ids[d] != g.id) {
        week_changes = true;
      }
      week_op.week.has_day[d] = true;
      week_op.week.day_ids[d] = g.slot >= 0 ? NEW_PLAN_ID : g.id;
    }
  }

  ops = unused_removes;
  ops.insert(ops.end(), adds.begin(), adds.end());
  ops.insert(ops.end(), before.begin(), before.end());
  if (week_changes)
    ops.push_back(week_op);
  ops.insert(ops.end(), after.begin(), after.end());
  ops.insert(ops.end(), removes.begin(), removes.end());
  return true;
}

bool plan_target(const Schedule &schedule, const ScheduleTarget &target, std::vector<PlanOp> &ops, std::string &error) {
  ops.clear();
  PlanOp op;
  switch (target.kind) {
    case ScheduleTarget::WEEK_TEXT:
      return plan_schedule_change(schedule, target.id, target.text, target.released, ops, error);
    case ScheduleTarget::DAY_PLAN: {
      const proto::DayPlan *have = schedule.day_plan(target.day.id);
      if (have == nullptr) {
        error = "no day plan with id " + std::to_string(target.day.id);
        return false;
      }
      if (proto::same_plan(*have, target.day) && have->name == target.day.name)
        return true;
      op.kind = PlanOp::SET_DAY;
      op.day = target.day;
      break;
    }
    case ScheduleTarget::WEEK_PLAN: {
      const proto::WeekPlan *have = schedule.week_plan(target.week.id);
      if (have == nullptr) {
        error = "no week plan with id " + std::to_string(target.week.id);
        return false;
      }
      for (uint32_t id : target.week.day_ids)
        if (schedule.day_plan(id) == nullptr) {
          error = "no day plan with id " + std::to_string(id);
          return false;
        }
      if (proto::same_plan(*have, target.week) && have->name == target.week.name)
        return true;
      op.kind = PlanOp::SET_WEEK;
      op.week = target.week;
      break;
    }
    case ScheduleTarget::REMOVE_DAY:
      if (schedule.day_plan(target.id) == nullptr)
        return true;
      for (auto &w : schedule.week_plans)
        for (int d = 0; d < 7; d++)
          if (w.has_day[d] && w.day_ids[d] == target.id) {
            error = "day plan " + std::to_string(target.id) + " is in use by week plan " + std::to_string(w.id);
            return false;
          }
      op.kind = PlanOp::REMOVE_DAY;
      op.id = target.id;
      break;
    case ScheduleTarget::REMOVE_WEEK:
      if (schedule.week_plan(target.id) == nullptr)
        return true;
      op.kind = PlanOp::REMOVE_WEEK;
      op.id = target.id;
      break;
    case ScheduleTarget::ADD_DAY:
      if (schedule.day_plans.size() >= MAX_DAY_PLANS) {
        error = "the remote holds at most 8 day plans";
        return false;
      }
      op.kind = PlanOp::ADD_DAY;
      op.day = target.day;
      op.day.id = NEW_PLAN_ID;
      op.slot = 0;
      break;
    case ScheduleTarget::ADD_WEEK:
      if (schedule.week_plans.size() >= MAX_WEEK_PLANS) {
        error = "the remote holds at most 3 week plans";
        return false;
      }
      for (uint32_t id : target.week.day_ids)
        if (schedule.day_plan(id) == nullptr) {
          error = "no day plan with id " + std::to_string(id);
          return false;
        }
      op.kind = PlanOp::ADD_WEEK;
      op.week = target.week;
      op.week.id = NEW_PLAN_ID;
      break;
  }
  ops.push_back(op);
  return true;
}

std::string describe_ops(const std::vector<PlanOp> &ops) {
  std::string out;
  for (auto &op : ops) {
    std::string what;
    switch (op.kind) {
      case PlanOp::SET_DAY:
        what = "day plan " + std::to_string(op.day.id);
        break;
      case PlanOp::ADD_DAY:
        what = "new day plan '" + op.day.name + "'";
        break;
      case PlanOp::REMOVE_DAY:
        what = "day plan " + std::to_string(op.id) + " still there";
        break;
      case PlanOp::SET_WEEK:
        what = "week plan " + std::to_string(op.week.id);
        break;
      case PlanOp::ADD_WEEK:
        what = "new week plan '" + op.week.name + "'";
        break;
      case PlanOp::REMOVE_WEEK:
        what = "week plan " + std::to_string(op.id) + " still there";
        break;
    }
    out += (out.empty() ? "" : ", ") + what;
  }
  return out;
}

}  // namespace esphome::zehnder_wivar
