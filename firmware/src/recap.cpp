#include "recap.h"
#include "pet.h"
#include "pet_screen.h"
#include "streak.h"
#include <Preferences.h>
#include <Arduino.h>
#include <string.h>

#define NVS_NAMESPACE_RECAP "recap"

// Throttle for g_today persists driven by XP/care events (P1-17). Without
// this, a power loss between the last save_nvs cadence and midnight rollover
// would lose the day's recap data; with a 60s throttle the worst-case window
// is ≤1 minute of activity.
#define TODAY_PERSIST_MIN_INTERVAL_MS 60000u

static DailySnapshot g_today   = {};
static DailySnapshot g_history[RECAP_HISTORY_DAYS] = {};
static uint8_t       g_history_count = 0;   // 0..RECAP_HISTORY_DAYS entries filled
static uint8_t       g_history_head  = 0;   // oldest slot index (ring-buffer head)
static uint32_t      g_last_shown_day = 0;
static uint32_t      g_last_today_persist_ms = 0;
static bool          g_today_dirty           = false;

static void persist(void) {
    Preferences prefs;
    if (prefs.begin(NVS_NAMESPACE_RECAP, false)) {
        prefs.putBytes("today",  &g_today,   sizeof(g_today));
        prefs.putBytes("hist",   g_history,  sizeof(g_history));
        prefs.putUChar("hcount", g_history_count);
        prefs.putUChar("hhead",  g_history_head);
        prefs.putUInt ("lshown", g_last_shown_day);
        prefs.end();
    }
    g_last_today_persist_ms = (uint32_t)millis();
    g_today_dirty           = false;
}

// Throttled persist for g_today XP/care events (P1-17). Defers when called
// more than once per TODAY_PERSIST_MIN_INTERVAL_MS; the dirty flag is picked
// up on the next eligible event or by recap_tick on day rollover (which
// calls persist() unconditionally).
static void persist_today_throttled(void) {
    uint32_t now = (uint32_t)millis();
    g_today_dirty = true;
    if (g_last_today_persist_ms != 0 &&
        (now - g_last_today_persist_ms) < TODAY_PERSIST_MIN_INTERVAL_MS) {
        return;
    }
    persist();
}

void recap_init(void) {
    Preferences prefs;
    if (prefs.begin(NVS_NAMESPACE_RECAP, true)) {
        prefs.getBytes("today",  &g_today,  sizeof(g_today));
        prefs.getBytes("hist",   g_history, sizeof(g_history));
        g_history_count  = prefs.getUChar("hcount", 0);
        g_history_head   = prefs.getUChar("hhead",  0);
        g_last_shown_day = prefs.getUInt ("lshown", 0);
        prefs.end();
    }
    // Clamp in case NVS held a corrupt value.
    if (g_history_count > RECAP_HISTORY_DAYS) g_history_count = RECAP_HISTORY_DAYS;
    if (g_history_head  >= RECAP_HISTORY_DAYS) g_history_head  = 0;
    Serial.printf("[recap] init: history=%u last_shown=%u today_day=%u\n",
                  (unsigned)g_history_count,
                  (unsigned)g_last_shown_day,
                  (unsigned)g_today.day_index);
}

// Write g_today into the ring buffer, then zero g_today.
// The care_streak_end / usage_streak_end fields are captured from the
// streak module at snapshot time.
static void snapshot_and_rotate(void) {
    uint8_t idx;
    if (g_history_count < RECAP_HISTORY_DAYS) {
        idx = g_history_count;
        g_history_count++;
    } else {
        // Ring is full — overwrite the oldest slot.
        idx = g_history_head;
        g_history_head = (g_history_head + 1) % RECAP_HISTORY_DAYS;
    }
    g_history[idx] = g_today;
    // Capture streak state at end-of-day.
    g_history[idx].care_streak_end  = streak_care_days();
    g_history[idx].usage_streak_end = streak_usage_days();

    Serial.printf("[recap] snapshot day=%u xp=%u care=%u ach=%u shiny=%u\n",
                  (unsigned)g_today.day_index,
                  (unsigned)g_today.xp_gained,
                  (unsigned)g_today.care_actions,
                  (unsigned)g_today.achievements_unlocked,
                  (unsigned)g_today.shinies_seen);

    memset(&g_today, 0, sizeof(g_today));
}

void recap_tick(void) {
    uint32_t today = pet_today_index();
    if (today == 0) return;          // no wall-clock yet

    if (g_today.day_index == 0) {
        // First tick with a valid clock — seed today's index.
        g_today.day_index = today;
        persist();
        return;
    }
    if (today == g_today.day_index) return;  // same day — nothing to do

    // Day has rolled over (possibly multiple days if device was off).
    // Snapshot the previous day; if more than one day skipped the gaps
    // record as empty entries (day_index=previous days with zero data).
    // For simplicity we just snapshot g_today (which may be stale) and
    // move on — the most recently recorded real data is preserved.
    snapshot_and_rotate();
    g_today.day_index = today;
    persist();
}

void recap_note_xp_gain(uint32_t xp) {
    recap_tick();   // ensure day_index is current
    if (g_today.day_index == 0) return;
    g_today.xp_gained += xp;
    persist_today_throttled();   // P1-17: ≤1min lossiness on power-cut
}

void recap_note_care_action(void) {
    recap_tick();
    if (g_today.day_index == 0) return;
    if (g_today.care_actions < 0xFFFF) g_today.care_actions++;
    persist_today_throttled();   // P1-17
}

void recap_note_achievement_unlock(void) {
    recap_tick();
    if (g_today.day_index == 0) return;
    if (g_today.achievements_unlocked < 0xFF) g_today.achievements_unlocked++;
    persist();   // rare event — persist immediately
}

void recap_note_shiny_discovered(void) {
    recap_tick();
    if (g_today.day_index == 0) return;
    if (g_today.shinies_seen < 0xFF) g_today.shinies_seen++;
    persist();   // rare event — persist immediately
}

const DailySnapshot* recap_yesterday(void) {
    if (g_history_count == 0) return nullptr;
    uint8_t last_idx;
    if (g_history_count < RECAP_HISTORY_DAYS) {
        // Buffer not yet wrapped — newest entry is at (count - 1).
        last_idx = g_history_count - 1;
    } else {
        // Buffer is full — head points to oldest; one slot before head is newest.
        last_idx = (g_history_head + RECAP_HISTORY_DAYS - 1) % RECAP_HISTORY_DAYS;
    }
    return &g_history[last_idx];
}

bool recap_should_show(void) {
    uint32_t today = pet_today_index();
    if (today == 0) return false;            // no wall-clock
    if (g_history_count == 0) return false;  // no yesterday entry yet
    if (today <= g_last_shown_day) return false;
    // Don't pop a modal full of zeros on a rest day (P1-16). The stats-screen
    // "Yesterday -" placeholder already conveys the no-data state; firing a
    // full modal for an empty snapshot undermines its "investment payoff" feel.
    const DailySnapshot* y = recap_yesterday();
    if (y && y->xp_gained == 0 && y->care_actions == 0 &&
        y->achievements_unlocked == 0 && y->shinies_seen == 0) {
        return false;
    }
    return true;
}

void recap_mark_shown(void) {
    g_last_shown_day = pet_today_index();
    persist();
    Serial.printf("[recap] marked shown for day=%u\n", (unsigned)g_last_shown_day);
}

void recap_force_reshow(void) {
    // Reset the once-per-day gate so a subsequent pet-screen entry would
    // also pop the modal organically, then directly invoke the show path
    // so the modal appears immediately regardless of current screen.
    g_last_shown_day = 0;
    persist();
    pet_screen_force_recap();
}

void recap_force_snapshot(void) {
    // Ensure day_index is valid before forcing.
    uint32_t today = pet_today_index();
    if (today == 0) {
        // No wall-clock — use a synthetic day index so testing still works.
        if (g_today.day_index == 0) g_today.day_index = 1;
    } else {
        g_today.day_index = today;
    }
    snapshot_and_rotate();
    g_today.day_index = (today > 0) ? today : g_today.day_index;
    persist();
}
