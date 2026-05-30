#pragma once
#include <stdint.h>

#define ACHIEVEMENTS_MAX 32   // bitfield width; current count <= 20 with headroom

// Each achievement has an ID (its bit position), name, description, and a
// pointer to a function that returns true when the achievement should fire.
typedef bool (*achievement_check_fn)(void);

typedef struct {
    uint8_t              id;             // 0..ACHIEVEMENTS_MAX-1
    const char*          name;
    const char*          description;
    achievement_check_fn check;
} Achievement;

extern const Achievement ACHIEVEMENTS[];
extern const uint8_t ACHIEVEMENTS_COUNT;

// Initialize: load unlocked bitmask from NVS.
void achievements_init(void);

// Evaluate all achievements; for any not-yet-unlocked one whose check returns
// true, set the bit and fire on_unlock callback. Call once per minute or so
// from pet_tick.
void achievements_tick(void);

// Query state.
uint32_t achievements_unlocked_mask(void);
uint8_t  achievements_unlocked_count(void);
bool     achievement_is_unlocked(uint8_t id);

// Force-unlock for testing. Sets the bit, persists, fires the callback.
void achievements_force_unlock(uint8_t id);

// Called synchronously from pet.cpp on each level-up. Use to evaluate
// level-threshold achievements before any side-effects (like graduation
// reset) can erase the trigger condition.
void achievements_notify_levelup(uint8_t new_level);

// Hook for UI: called when a new achievement fires. Set once at startup.
typedef void (*achievement_unlock_cb_t)(const Achievement* a);
void achievements_set_unlock_cb(achievement_unlock_cb_t cb);

// Weekly quest: rotates each week. Returns the active quest's achievement-id.
// Uses pet_today_index() for week calculation; returns WEEKLY_QUEST_BASE if no wall-clock yet.
uint8_t weekly_quest_active_id(void);

// All achievements with id < WEEKLY_QUEST_BASE are regular; >= are pool
// for weekly quests.
#define WEEKLY_QUEST_BASE 16   // achievements 0..15 are regular; 16..23 are quest pool
