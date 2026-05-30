#include "achievements.h"
#include "pet.h"
#include "recap.h"
#include <Preferences.h>
#include <Arduino.h>

#define NVS_NAMESPACE_ACH "ach"
#define NVS_KEY_UNLOCKED  "u"

static uint32_t g_unlocked = 0;
static achievement_unlock_cb_t g_unlock_cb = nullptr;
static uint32_t g_last_tick_ms = 0;

// ============== Evaluator functions ==============

// Level milestones
static bool ach_lv5(void)  { const Pet* p = pet_current(); return p && p->level >= 5; }
static bool ach_lv10(void) { const Pet* p = pet_current(); return p && p->level >= 10; }
static bool ach_lv20(void) { const Pet* p = pet_current(); return p && p->level >= 20; }
static bool ach_lv30(void) { const Pet* p = pet_current(); return p && p->level >= 30; }

// Care milestones — delegate to pet lifetime counters
static bool ach_feed_50(void)  { return pet_lifetime_feed_count() >= 50; }
static bool ach_play_50(void)  { return pet_lifetime_play_count() >= 50; }
static bool ach_pet_100(void)  { return pet_lifetime_pet_count() >= 100; }

// Variety / discovery
static bool ach_first_shiny(void) {
    return pet_discovered_shiny_count() >= 1;
}
static bool ach_three_species(void) {
    return pet_discovered_count() >= 3;
}
static bool ach_all_species(void) {
    return pet_discovered_count() >= PET_NUM_SPECIES;
}
static bool ach_first_graduate(void) {
    const Gallery* g = pet_gallery();
    return g && g->count >= 1;
}
static bool ach_five_graduates(void) {
    const Gallery* g = pet_gallery();
    return g && g->count >= 5;
}

// Heroics
static bool ach_vacation_used(void) {
    // Latches when vacation has ever been activated. Uses a static flag so
    // it stays true even after vacation is turned off — the achievement is
    // "used it once", not "currently on vacation".
    static bool seen = false;
    if (!seen && pet_is_vacation_active()) seen = true;
    return seen;
}
static bool ach_naming_done(void) {
    const Pet* p = pet_current();
    if (!p) return false;
    const char* species = pet_species_name(p->species_id);
    return strcmp(p->name, species) != 0;
}

// ============== Weekly quest pool evaluators ==============
// Only the currently active quest is evaluated each tick.

static bool quest_5_feeds_this_week(void) {
    return pet_week_feed_count() >= 5;
}
static bool quest_lvl_up_2_this_week(void) {
    return pet_week_levelups() >= 2;
}
static bool quest_play_5_this_week(void) {
    return pet_week_play_count() >= 5;
}
static bool quest_discover_species_this_week(void) {
    return pet_week_species_discovered() >= 1;
}

// ============== Achievement table ==============

const Achievement ACHIEVEMENTS[] = {
    // Regular achievements (ids 0..15)
    {0,  "First Steps",      "Pet reaches Lv 5",                 ach_lv5},
    {1,  "Growing Strong",   "Pet reaches Lv 10",                ach_lv10},
    {2,  "Veteran",          "Pet reaches Lv 20",                ach_lv20},
    {3,  "Graduate",         "Pet reaches Lv 30",                ach_lv30},
    {4,  "Well Fed",         "Feed 50 times",                    ach_feed_50},
    {5,  "Playmate",         "Play 50 times",                    ach_play_50},
    {6,  "Affectionate",     "Pet 100 times",                    ach_pet_100},
    {7,  "Shiny Hunter",     "Discover your first shiny",        ach_first_shiny},
    {8,  "Variety Pack",     "Discover 3 species",               ach_three_species},
    {9,  "Completionist",    "Discover all 13 species",          ach_all_species},
    {10, "Send-off",         "Graduate your first pet",          ach_first_graduate},
    {11, "Old Soul",         "Graduate 5 pets",                  ach_five_graduates},
    {12, "Took a Break",     "Use vacation mode once",           ach_vacation_used},
    {13, "Identity",         "Give your pet a custom name",      ach_naming_done},
    // ids 14, 15 reserved for future regular achievements

    // Weekly quest pool (ids 16..19) — only one active per week
    {16, "Quest: Snack Pack",  "Feed 5 times this week",         quest_5_feeds_this_week},
    {17, "Quest: Climb",       "Gain 2 levels this week",        quest_lvl_up_2_this_week},
    {18, "Quest: Fun Time",    "Play 5 times this week",         quest_play_5_this_week},
    {19, "Quest: Explorer",    "Discover a new species this week", quest_discover_species_this_week},
    // ids 20..23 reserved for future quests
};
const uint8_t ACHIEVEMENTS_COUNT = sizeof(ACHIEVEMENTS) / sizeof(ACHIEVEMENTS[0]);

// ============== Persistence ==============

static void persist(void) {
    Preferences prefs;
    if (prefs.begin(NVS_NAMESPACE_ACH, false)) {
        prefs.putUInt(NVS_KEY_UNLOCKED, g_unlocked);
        prefs.end();
    }
}

// ============== Public API ==============

void achievements_init(void) {
    Preferences prefs;
    if (prefs.begin(NVS_NAMESPACE_ACH, true)) {
        g_unlocked = prefs.getUInt(NVS_KEY_UNLOCKED, 0);
        prefs.end();
    }
    g_last_tick_ms = millis();
    Serial.printf("[ach] init: unlocked_mask=0x%08X (%u total)\n",
                  (unsigned)g_unlocked, (unsigned)achievements_unlocked_count());
}

void achievements_tick(void) {
    // Throttle: evaluate at most every 60 seconds.
    if ((millis() - g_last_tick_ms) < 60000) return;
    g_last_tick_ms = millis();

    uint8_t active_quest = weekly_quest_active_id();

    for (uint8_t i = 0; i < ACHIEVEMENTS_COUNT; i++) {
        const Achievement* a = &ACHIEVEMENTS[i];
        uint32_t bit = 1u << a->id;
        if (g_unlocked & bit) continue;              // already unlocked
        // For weekly quests, only evaluate the currently-active one.
        if (a->id >= WEEKLY_QUEST_BASE && a->id != active_quest) continue;
        if (a->check && a->check()) {
            g_unlocked |= bit;
            persist();
            recap_note_achievement_unlock();
            Serial.printf("[ach] unlocked: %s — %s\n", a->name, a->description);
            if (g_unlock_cb) g_unlock_cb(a);
        }
    }
}

uint32_t achievements_unlocked_mask(void)   { return g_unlocked; }

bool achievement_is_unlocked(uint8_t id)    {
    if (id >= ACHIEVEMENTS_MAX) return false;
    return (g_unlocked & (1u << id)) != 0;
}

uint8_t achievements_unlocked_count(void) {
    return (uint8_t)__builtin_popcount(g_unlocked);
}

void achievements_set_unlock_cb(achievement_unlock_cb_t cb) {
    g_unlock_cb = cb;
}

void achievements_force_unlock(uint8_t id) {
    if (id >= ACHIEVEMENTS_MAX) return;
    uint32_t bit = 1u << id;
    if (g_unlocked & bit) {
        Serial.printf("[ach] force_unlock: id=%u already unlocked\n", (unsigned)id);
        return;
    }
    g_unlocked |= bit;
    persist();
    recap_note_achievement_unlock();
    // Find the achievement definition to pass to the callback.
    for (uint8_t i = 0; i < ACHIEVEMENTS_COUNT; i++) {
        if (ACHIEVEMENTS[i].id == id) {
            Serial.printf("[ach] force_unlock: %s — %s\n",
                          ACHIEVEMENTS[i].name, ACHIEVEMENTS[i].description);
            if (g_unlock_cb) g_unlock_cb(&ACHIEVEMENTS[i]);
            return;
        }
    }
    Serial.printf("[ach] force_unlock: id=%u (no definition found)\n", (unsigned)id);
}

void achievements_notify_levelup(uint8_t new_level) {
    // Match against the achievements that key off Pet level.
    // We can't trust the check fn (which reads pet_current()->level) at the
    // moment of graduation, so we evaluate the level threshold directly here.
    static const struct { uint8_t id; uint8_t lvl; } level_aches[] = {
        {0, 5}, {1, 10}, {2, 20}, {3, 30}
    };
    for (auto& la : level_aches) {
        if (new_level >= la.lvl && !achievement_is_unlocked(la.id)) {
            achievements_force_unlock(la.id);
        }
    }
}

uint8_t weekly_quest_active_id(void) {
    uint32_t day = pet_today_index();
    if (day == 0) return WEEKLY_QUEST_BASE;  // no wall-clock yet — default to first quest
    uint32_t week = day / 7;
    // Quests pool is ids 16..19 (4 entries); rotate weekly.
    return (uint8_t)(WEEKLY_QUEST_BASE + (week % 4));
}
