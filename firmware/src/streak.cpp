#include "streak.h"
#include "pet.h"
#include <Preferences.h>
#include <Arduino.h>

#define NVS_NAMESPACE_STRK "strk"
// Keys:
//   cd = care current days (uint16)
//   cb = care best (uint16)
//   ud = usage current days
//   ub = usage best
//   ld = last_day_index (uint32) — the pet_today_index seen when we last updated
//   ca = care_active_today  (uint8 bool)
//   ua = usage_active_today (uint8 bool)

static uint16_t g_care_days          = 0;
static uint16_t g_care_best          = 0;
static uint16_t g_usage_days         = 0;
static uint16_t g_usage_best         = 0;
static uint32_t g_last_day_idx       = 0;
static bool     g_care_active_today  = false;
static bool     g_usage_active_today = false;
static bool     g_dirty              = false;

// Minimum interval between routine NVS persists. Day-rollover and first-time
// anchoring bypass this via persist_now() to avoid losing new streak values
// on an immediate power cycle.
#define STREAK_PERSIST_MIN_INTERVAL_MS 60000

static uint32_t g_last_persist_ms = 0;

// Internal: write to NVS unconditionally (bypasses throttle).
static void do_persist(void) {
    Preferences prefs;
    if (prefs.begin(NVS_NAMESPACE_STRK, false)) {
        prefs.putUShort("cd", g_care_days);
        prefs.putUShort("cb", g_care_best);
        prefs.putUShort("ud", g_usage_days);
        prefs.putUShort("ub", g_usage_best);
        prefs.putUInt  ("ld", g_last_day_idx);
        prefs.putUChar ("ca", g_care_active_today ? 1 : 0);
        prefs.putUChar ("ua", g_usage_active_today ? 1 : 0);
        prefs.end();
        g_dirty = false;
        g_last_persist_ms = millis();
    }
}

// Throttled persist: at most once per STREAK_PERSIST_MIN_INTERVAL_MS.
// Called from streak_note_care_action / streak_note_claude_session so that
// care-action bursts don't each open a separate Preferences handle.
static void persist(void) {
    if (!g_dirty) return;
    uint32_t now = millis();
    if (now - g_last_persist_ms < STREAK_PERSIST_MIN_INTERVAL_MS) return;
    do_persist();
}

// Unthrottled persist: used after day rollovers and first-time anchoring
// where losing the updated value to a power cycle would break the streak.
static void persist_now(void) {
    if (!g_dirty) return;
    do_persist();
}

void streak_init(void) {
    Preferences prefs;
    if (prefs.begin(NVS_NAMESPACE_STRK, true)) {
        g_care_days          = prefs.getUShort("cd", 0);
        g_care_best          = prefs.getUShort("cb", 0);
        g_usage_days         = prefs.getUShort("ud", 0);
        g_usage_best         = prefs.getUShort("ub", 0);
        g_last_day_idx       = prefs.getUInt  ("ld", 0);
        g_care_active_today  = prefs.getUChar ("ca", 0) != 0;
        g_usage_active_today = prefs.getUChar ("ua", 0) != 0;
        prefs.end();
    }
    Serial.printf("[streak] loaded: care=%u/%u use=%u/%u day=%u\n",
                  (unsigned)g_care_days, (unsigned)g_care_best,
                  (unsigned)g_usage_days, (unsigned)g_usage_best,
                  (unsigned)g_last_day_idx);
}

void streak_tick(void) {
    uint32_t today = pet_today_index();
    if (today == 0) return;                              // no wall-clock yet
    if (today == g_last_day_idx) return;                 // same day, nothing to do
    if (g_last_day_idx == 0) {
        // First time seeing wall-clock — anchor without breaking streaks
        g_last_day_idx = today;
        g_dirty = true;
        persist_now();
        return;
    }

    // Vacation mode: advance the day index but PAUSE streaks — neither
    // increment (no credit for inactive vacation days) nor decrement (no
    // penalty for not caring during vacation). The streak value is frozen at
    // its pre-vacation level and resumes counting from there on return.
    if (pet_is_vacation_active()) {
        g_last_day_idx       = today;
        g_care_active_today  = false;
        g_usage_active_today = false;
        g_dirty = true;
        persist_now();
        return;
    }

    // We've rolled over one or more days. Walk forward day-by-day:
    //   - If yesterday was active → streak += 1 (capped at uint16 max)
    //   - If yesterday was inactive → streak = 0
    // The flags g_care_active_today / g_usage_active_today represent the
    // immediately-previous day's state at this rollover point.
    uint32_t gap = today - g_last_day_idx;
    for (uint32_t d = 0; d < gap; d++) {
        bool care_was_active  = (d == 0) ? g_care_active_today  : false;
        bool usage_was_active = (d == 0) ? g_usage_active_today : false;
        if (care_was_active) {
            if (g_care_days < 65535) g_care_days++;
            if (g_care_days > g_care_best) g_care_best = g_care_days;
        } else {
            g_care_days = 0;
        }
        if (usage_was_active) {
            if (g_usage_days < 65535) g_usage_days++;
            if (g_usage_days > g_usage_best) g_usage_best = g_usage_days;
        } else {
            g_usage_days = 0;
        }
    }
    g_care_active_today  = false;
    g_usage_active_today = false;
    g_last_day_idx = today;
    g_dirty = true;
    persist_now();
}

void streak_note_care_action(void) {
    streak_tick();                         // ensure day flags are fresh
    if (!g_care_active_today) {
        g_care_active_today = true;
        g_dirty = true;
        persist();
    }
}

void streak_note_claude_session(void) {
    streak_tick();
    if (!g_usage_active_today) {
        g_usage_active_today = true;
        g_dirty = true;
        persist();
    }
}

uint16_t streak_care_days(void)  { return g_care_days; }
uint16_t streak_usage_days(void) { return g_usage_days; }
uint16_t streak_care_best(void)  { return g_care_best; }
uint16_t streak_usage_best(void) { return g_usage_best; }

void streak_test_advance_days(uint16_t n) {
    // Subtract n from the last_day_idx so streak_tick sees a gap of n days.
    if (g_last_day_idx >= (uint32_t)n) {
        g_last_day_idx -= (uint32_t)n;
    } else {
        g_last_day_idx = 0;
    }
    streak_tick();
}

void streak_test_set_days(uint16_t n) {
    g_care_days  = n;
    g_usage_days = n;
    if (n > g_care_best)  g_care_best  = n;
    if (n > g_usage_best) g_usage_best = n;
    g_dirty = true;
    persist_now();   // test helper — persist immediately so serial-cmd users see the value stick
    Serial.printf("[streak] test_set_days: care=%u use=%u\n",
                  (unsigned)g_care_days, (unsigned)g_usage_days);
}
