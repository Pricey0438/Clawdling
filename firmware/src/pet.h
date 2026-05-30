#pragma once
#include <stdint.h>
#include "data.h"   // for UsageData, SessionList, SESSION_MAX

// ============== Tunables (generated from firmware/balance.yaml) ==============
// All constants below come from the codegen step (firmware/scripts/gen_balance_h.py).
// Edit firmware/balance.yaml and rebuild — do not add raw #defines here.
#include "pet_balance_generated.h"

// ============== Types ==============
typedef struct {
    uint16_t species_id;
    uint8_t  level;            // 1..PET_MAX_LEVEL
    uint32_t xp;               // XP within current level
    uint32_t born_ts;          // pet_now() at creation
    uint32_t last_event_ts;    // pet_now() of last XP gain — kept for NVS compat / telemetry; no longer drives decay
    // Care stats (phase 3) — each 0..100
    uint8_t  satiety;
    uint8_t  spirit;
    uint8_t  bond;
    // Decay accounting — each stat advances its own clock so refills "freshen" decay
    uint32_t satiety_decay_ts;
    uint32_t spirit_decay_ts;
    uint32_t bond_decay_ts;
    // V4 additions:
    char     name[16];   // null-terminated, default = species name on hatch
    uint8_t  is_shiny;   // 0 or 1
    uint8_t  _pad[3];                  // padding: aligns last_level_up_pet_now to 32-bit boundary
    // Runtime-only (not persisted) — set to pet_now() at each level-up event.
    // Zero means no level-up has occurred since boot.
    uint32_t last_level_up_pet_now;
} Pet;

typedef struct {
    uint16_t species_id;
    uint32_t born_ts;
    uint32_t graduated_ts;
    uint32_t total_xp_earned;
    // V4 additions:
    char     name[16];
    uint8_t  is_shiny;
    uint8_t  _pad[3];
    // V5 additions (F12 bio-card): captured at graduate-time so the
    // retirement-home bio modal can render a 'signature quote' without
    // any daemon round-trip. Empty when the pet had no speech bubble
    // showing at graduation (e.g. when speech was muted via settings).
    char     quote[64];
} Graduate;

typedef struct {
    uint16_t count;            // 0..PET_GALLERY_CAP
    Graduate entries[PET_GALLERY_CAP];
} Gallery;

typedef enum {
    PET_EVENT_LEVEL_UP,
    PET_EVENT_EVOLVE,
    PET_EVENT_GRADUATE,
    PET_EVENT_NEGLECTED,        // phase 3 — fires when any stat hits 0
    PET_EVENT_HATCHED,          // fires at the end of instantiate_pet() for brand-new pets
} pet_event_t;

// ============== PetStats (phase 3) ==============
// Read-only snapshot for the screen. All three cooldown fields are
// computed at call time as max(0, ACTION_COOLDOWN_SEC - (pet_now() -
// last_action_ts)) so the screen can render greyed buttons without
// separately tracking time.
typedef struct {
    uint8_t  satiety;
    uint8_t  spirit;
    uint8_t  bond;
    uint32_t feed_cooldown_remaining;   // seconds; 0 = ready
    uint32_t play_cooldown_remaining;
    uint32_t pet_cooldown_remaining;
} PetStats;

// ============== Public interface ==============
void           pet_init(void);                                // load from NVS or auto-create
void           pet_tick(void);                                // main-loop call: idle decay + NVS flush cadence
void           pet_on_metrics(const UsageData* u,
                              const SessionList* s);          // BLE parse-success hook
const Pet*     pet_current(void);                             // read-only accessor
const Gallery* pet_gallery(void);                             // read-only accessor
uint8_t        pet_evolution_stage(const Pet* p);             // 0..4 sprite tier from level
uint32_t       pet_xp_to_next(uint8_t level);                 // curve helper for UI bars
void           pet_set_event_cb(void (*cb)(pet_event_t));     // UI sub-projects register here
uint8_t        pet_species_animation_index(uint16_t species_id);  // species → claudepix index (wraps mod PET_NUM_SPECIES)
const char*    pet_species_name(uint16_t species_id);             // code-pun creature name; string literal, do not free
uint32_t       pet_total_xp_earned(void);                          // current xp + cost of all completed levels

// Seconds (in pet_now() timescale) since the most recent level-up event this
// boot session. Returns 0 if no level-up has been recorded since boot — caller
// checks `pet_current()->level > 1` separately to decide whether to show the line.
uint32_t pet_time_since_level_up(void);

// Monotonic clock in seconds since first boot (never resets during a power-on cycle).
// Exposed so modules outside pet.cpp can compute age/elapsed-time deltas.
uint32_t pet_now(void);

// Wall-clock from daemon. Returns 0 if never received a payload with `ts`.
uint32_t pet_wallclock_now(void);
void     pet_set_wallclock(uint32_t epoch);
void     pet_set_tz_offset(int32_t offset_sec);
uint32_t pet_today_index(void);   // local-day index = (wallclock + tz)/86400; 0 if no wallclock

// Rename the current pet. Copies up to 15 characters of `name`, null-terminates,
// and triggers an immediate NVS flush so the name survives a power cycle.
// Safe to call from LVGL callbacks. Empty string is silently ignored.
void pet_rename(const char* name);

// Copy the current pet's name into `dst` (always null-terminated).
// `cap` is the size of dst including the null terminator. If cap is 0,
// no-op. Returns the length actually copied (excluding terminator).
size_t pet_get_name(char* dst, size_t cap);

// Toggle shiny flag on the current pet. Marks dirty (NVS flushes on next tick).
// Intended for serial cmd testing; shininess is normally set at hatch (1/64 chance).
void pet_set_shiny(bool on);

// Care actions (phase 3). Each returns true on success; false if the
// action is on cooldown OR the corresponding stat is already at 100.
bool pet_feed(void);
bool pet_play(void);
bool pet_pet(void);

// Snapshot of care state including computed cooldown remaining.
PetStats pet_stats(void);

// Vacation mode pauses care decay. Auto-expires after 14 days.
bool     pet_is_vacation_active(void);
uint32_t pet_vacation_days_remaining(void);  // 0 if not active
void     pet_set_vacation(bool active);

// Activate vacation with a specific remaining-day count (1..VACATION_MAX_DAYS,
// clamped). Backdates the start timestamp so pet_vacation_days_remaining()
// reports `days`. Used by the `sim vacation N` serial cmd to capture
// vacation-banner UI states for any day count without waiting 14 days.
void     pet_vacation_start(uint32_t days);

// Species catalog discovery — bitmask, bit N = species N has ever been owned.
// "Owned" means: current pet IS this species OR any graduate IS this species.
uint16_t pet_discovered_species_mask(void);
uint16_t pet_discovered_shiny_mask(void);   // bit N = shiny variant of species N has been seen
void     pet_mark_species_discovered(uint16_t species_id, bool shiny);
uint8_t  pet_discovered_count(void);          // popcount of regular discovery mask
uint8_t  pet_discovered_shiny_count(void);    // popcount of shiny mask

// ============== Achievement counter accessors (task 2.3) ==============
// Lifetime: cumulative across all pets; persisted in NVS.
uint32_t pet_lifetime_feed_count(void);
uint32_t pet_lifetime_play_count(void);
uint32_t pet_lifetime_pet_count(void);

// Week-scoped: reset each Monday (or whenever pet_today_index() / 7 changes).
// Not persisted — acceptable to lose on reboot within the same week.
uint32_t pet_week_feed_count(void);
uint32_t pet_week_play_count(void);
uint32_t pet_week_levelups(void);
uint32_t pet_week_species_discovered(void);

// ============== Test / dev helpers ==============
// Populate n fake graduates into the gallery (rotating species, random shinies).
// Persists via save_nvs(). Keep this permanently — used for screenshot QA.
void pet_test_populate_graduates(uint8_t n);

// Wipe the current pet from NVS and instantiate a fresh L1 pet (next species
// in the round-robin). Fires PET_EVENT_HATCHED so the naming keyboard pops.
// Used by the `sim hatch` serial cmd to capture the hatch flow on demand.
void pet_test_clear_and_rehatch(void);

// Force-graduate the current pet (snapshot to gallery, spawn next species,
// fire PET_EVENT_GRADUATE). Used by the `sim graduate` serial cmd.
void pet_force_graduate(void);
