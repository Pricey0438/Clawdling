#pragma once
#include <stdint.h>

#define RECAP_HISTORY_DAYS 7

typedef struct {
    uint32_t day_index;             // pet_today_index() value for this entry
    uint32_t xp_gained;             // XP earned on this day
    uint16_t care_actions;          // feed + play + pet count on this day
    uint16_t care_streak_end;       // care streak at end of day
    uint16_t usage_streak_end;      // usage streak at end of day
    uint8_t  achievements_unlocked; // count of new unlocks on this day
    uint8_t  shinies_seen;          // shinies discovered on this day
    uint8_t  _pad[2];
} DailySnapshot;

// Initialize — load from NVS. Call from setup() after pet_init().
void recap_init(void);

// Per-tick aggregation. Call from the main loop (or pet_tick path).
// Detects day rollover and snapshots the previous day before resetting
// today's counters.
void recap_tick(void);

// Hooks — call from the corresponding event paths.
void recap_note_xp_gain(uint32_t xp);
void recap_note_care_action(void);
void recap_note_achievement_unlock(void);
void recap_note_shiny_discovered(void);

// Query — for the recap modal display.
const DailySnapshot* recap_yesterday(void);  // nullptr if no yesterday snapshot
bool                 recap_should_show(void); // true if today_index > last_shown_index
void                 recap_mark_shown(void);  // call after the modal is dismissed

// Test helper — forces a snapshot of today into the ring buffer then
// resets today's counters. Exposed for the `recap_snap` serial cmd.
void recap_force_snapshot(void);

// Force-show the daily recap modal regardless of the once-per-day gate.
// Thin wrapper around the pet-screen modal entrypoint, kept here so the
// `sim recap-now` serial cmd can stay decoupled from pet_screen internals.
void recap_force_reshow(void);
