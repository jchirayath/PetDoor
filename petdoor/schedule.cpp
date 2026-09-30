#include "schedule.h"

#include <Preferences.h>

namespace Schedule {
namespace {

constexpr const char *kNvs = "petdoor-sched";
Window g_win[SCHEDULE_MAX_WINDOWS];
uint8_t g_count = 0;
int16_t g_offsetMin = SCHEDULE_UTC_OFFSET_MIN;

constexpr uint16_t kMinutesPerDay = 1440;

void persist() {
  Preferences p;
  if (!p.begin(kNvs, /*readOnly=*/false)) return;
  p.putShort("off", g_offsetMin);
  p.putUChar("n", g_count);
  p.putBytes("win", g_win, sizeof(Window) * g_count);
  p.end();
}

// Local wall time, as minutes since midnight and a day of week with 0 = Sunday.
void localise(uint32_t epochSec, uint16_t &minOfDay, uint8_t &dow) {
  // Signed, because a negative offset near the epoch would wrap an unsigned.
  const int64_t local = static_cast<int64_t>(epochSec) + g_offsetMin * 60;
  int64_t days = local / 86400;
  int64_t rem = local % 86400;
  if (rem < 0) { rem += 86400; days -= 1; }   // C division truncates toward zero
  minOfDay = static_cast<uint16_t>(rem / 60);
  // 1970-01-01 was a Thursday; +4 puts Sunday at 0.
  dow = static_cast<uint8_t>(((days % 7) + 7 + 4) % 7);
}

bool windowCovers(const Window &w, uint16_t minOfDay, uint8_t dow) {
  if (!w.enabled) return false;
  if (w.startMin < w.endMin) {
    // Ordinary same-day window.
    return (w.days & (1 << dow)) && minOfDay >= w.startMin && minOfDay < w.endMin;
  }
  // Crosses midnight. Two ways to be inside it: after the start today, or
  // before the end today because it started YESTERDAY — which is why the day
  // mask is tested against yesterday in the second case. Getting this wrong
  // makes a Friday-night window stop existing at midnight.
  if ((w.days & (1 << dow)) && minOfDay >= w.startMin) return true;
  const uint8_t yesterday = static_cast<uint8_t>((dow + 6) % 7);
  return (w.days & (1 << yesterday)) && minOfDay < w.endMin;
}

const char *kDayName[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

}  // namespace

void begin() {
  Preferences p;
  if (!p.begin(kNvs, /*readOnly=*/true)) return;
  g_offsetMin = p.getShort("off", SCHEDULE_UTC_OFFSET_MIN);
  const uint8_t n = p.getUChar("n", 0);
  if (n > 0 && n <= SCHEDULE_MAX_WINDOWS) {
    const size_t want = sizeof(Window) * n;
    if (p.getBytesLength("win") == want && p.getBytes("win", g_win, want) == want) {
      g_count = n;
    }
  }
  p.end();
}

uint8_t count() { return g_count; }
uint8_t capacity() { return SCHEDULE_MAX_WINDOWS; }

bool get(uint8_t index, Window &out) {
  if (index >= g_count) return false;
  out = g_win[index];
  return true;
}

bool add(uint16_t startMin, uint16_t endMin, uint8_t days, const char *&problem) {
  if (g_count >= SCHEDULE_MAX_WINDOWS) {
    problem = "no room for another window";
    return false;
  }
  if (startMin >= kMinutesPerDay || endMin >= kMinutesPerDay) {
    problem = "times must be between 00:00 and 23:59";
    return false;
  }
  if (startMin == endMin) {
    // Refused rather than guessed. It could mean "no time at all" or "the whole
    // day", and picking one silently would eventually lock a door for 24 hours
    // because somebody meant the other.
    problem = "start and end are the same; for a whole day use 00:00-23:59";
    return false;
  }
  if ((days & 0x7F) == 0) {
    problem = "no days selected";
    return false;
  }
  g_win[g_count].startMin = startMin;
  g_win[g_count].endMin = endMin;
  g_win[g_count].days = days & 0x7F;
  g_win[g_count].enabled = 1;
  g_count++;
  persist();
  problem = nullptr;
  return true;
}

bool removeAt(uint8_t index) {
  if (index >= g_count) return false;
  for (uint8_t i = index; i + 1 < g_count; i++) g_win[i] = g_win[i + 1];
  g_count--;
  persist();
  return true;
}

void clear() {
  g_count = 0;
  persist();
}

void setUtcOffsetMinutes(int16_t minutes) {
  if (minutes < -840 || minutes > 840) return;  // ±14 h covers every real zone
  g_offsetMin = minutes;
  persist();
}

int16_t utcOffsetMinutes() { return g_offsetMin; }

bool lockedNow(bool haveClock, uint32_t epochSec, uint8_t *which) {
  // The safety rule. A door that guesses the time can lock an animal out at
  // noon believing it is midnight, so an unknown clock means no schedule at
  // all rather than a schedule applied to a guess.
  if (!haveClock || g_count == 0) return false;
  uint16_t minOfDay;
  uint8_t dow;
  localise(epochSec, minOfDay, dow);
  for (uint8_t i = 0; i < g_count; i++) {
    if (windowCovers(g_win[i], minOfDay, dow)) {
      if (which != nullptr) *which = i;
      return true;
    }
  }
  return false;
}

uint32_t secondsUntilChange(bool haveClock, uint32_t epochSec) {
  if (!haveClock || g_count == 0) return 0;
  uint16_t minOfDay;
  uint8_t dow;
  localise(epochSec, minOfDay, dow);
  const bool nowLocked = lockedNow(haveClock, epochSec, nullptr);
  // Walk forward a minute at a time for at most a week. Crude, and it runs
  // once per status print rather than in any hot path; a closed-form answer
  // across day-mask boundaries and midnight wrap is far easier to get wrong
  // than it is to make fast.
  for (uint32_t ahead = 1; ahead <= 7 * 24 * 60; ahead++) {
    const uint32_t m = (minOfDay + ahead) % kMinutesPerDay;
    const uint8_t d = static_cast<uint8_t>((dow + ((minOfDay + ahead) / kMinutesPerDay)) % 7);
    bool locked = false;
    for (uint8_t i = 0; i < g_count && !locked; i++) {
      locked = windowCovers(g_win[i], static_cast<uint16_t>(m), d);
    }
    if (locked != nowLocked) return ahead * 60;
  }
  return 0;
}

bool parseSpec(const char *spec, Window &out, const char *&problem) {
  // "HH:MM-HH:MM" optionally followed by days: "Mon-Fri", "Sat,Sun", "daily".
  unsigned sh, sm, eh, em;
  int consumed = 0;
  if (sscanf(spec, "%u:%u-%u:%u%n", &sh, &sm, &eh, &em, &consumed) != 4) {
    problem = "expected HH:MM-HH:MM";
    return false;
  }
  if (sh > 23 || eh > 23 || sm > 59 || em > 59) {
    problem = "hours are 0-23 and minutes 0-59";
    return false;
  }
  out.startMin = static_cast<uint16_t>(sh * 60 + sm);
  out.endMin = static_cast<uint16_t>(eh * 60 + em);
  out.enabled = 1;
  out.days = 0x7F;

  const char *rest = spec + consumed;
  while (*rest == ' ' || *rest == '\t') rest++;
  if (*rest == '\0') { problem = nullptr; return true; }

  if (strcasecmp(rest, "daily") == 0 || strcasecmp(rest, "all") == 0) {
    problem = nullptr;
    return true;
  }

  uint8_t mask = 0;
  const char *p = rest;
  while (*p != '\0') {
    while (*p == ',' || *p == ' ') p++;
    if (*p == '\0') break;
    int first = -1;
    for (int d = 0; d < 7; d++) {
      if (strncasecmp(p, kDayName[d], 3) == 0) { first = d; break; }
    }
    if (first < 0) { problem = "day names are Sun Mon Tue Wed Thu Fri Sat"; return false; }
    p += 3;
    if (*p == '-') {
      p++;
      int last = -1;
      for (int d = 0; d < 7; d++) {
        if (strncasecmp(p, kDayName[d], 3) == 0) { last = d; break; }
      }
      if (last < 0) { problem = "day names are Sun Mon Tue Wed Thu Fri Sat"; return false; }
      p += 3;
      for (int d = first;; d = (d + 1) % 7) {   // wraps, so Fri-Mon works
        mask |= (1 << d);
        if (d == last) break;
      }
    } else {
      mask |= (1 << first);
    }
  }
  if (mask == 0) { problem = "no days recognised"; return false; }
  out.days = mask;
  problem = nullptr;
  return true;
}

void describe(Stream &out, bool haveClock, uint32_t epochSec) {
  if (g_count == 0) {
    out.println(F("  schedule     : none — the beacon may open the door at any hour"));
    return;
  }
  const int16_t off = g_offsetMin;
  out.printf("  schedule     : %u window%s, local time = UTC%+d:%02u\r\n",
             g_count, g_count == 1 ? "" : "s", off / 60, abs(off % 60));
  for (uint8_t i = 0; i < g_count; i++) {
    const Window &w = g_win[i];
    char days[32] = {0};
    if ((w.days & 0x7F) == 0x7F) {
      strcpy(days, "every day");
    } else {
      for (uint8_t d = 0; d < 7; d++) {
        if (w.days & (1 << d)) {
          if (days[0]) strncat(days, " ", sizeof(days) - strlen(days) - 1);
          strncat(days, kDayName[d], sizeof(days) - strlen(days) - 1);
        }
      }
    }
    out.printf("    [%u] %02u:%02u-%02u:%02u  %s%s\r\n", i,
               w.startMin / 60, w.startMin % 60, w.endMin / 60, w.endMin % 60,
               days, w.startMin > w.endMin ? "  (overnight)" : "");
  }
  if (!haveClock) {
    // Stated loudly, because this is the difference between a door that will
    // lock tonight and one that never will.
    out.println(F("    !! NO CLOCK YET — every window above is INERT until NTP answers."));
    return;
  }
  uint8_t which = 0;
  const bool locked = lockedNow(true, epochSec, &which);
  const uint32_t secs = secondsUntilChange(true, epochSec);
  if (locked) {
    out.printf("    LOCKED NOW by window [%u]%s\r\n", which,
               secs ? "" : "");
    if (secs) out.printf("    opens again in %lu h %lu min\r\n",
                         static_cast<unsigned long>(secs / 3600),
                         static_cast<unsigned long>((secs / 60) % 60));
  } else if (secs) {
    out.printf("    not locked; next window starts in %lu h %lu min\r\n",
               static_cast<unsigned long>(secs / 3600),
               static_cast<unsigned long>((secs / 60) % 60));
  }
}

}  // namespace Schedule
