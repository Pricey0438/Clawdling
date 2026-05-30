#include "pet.h"
#include "achievements.h"
#include "splash.h"
#include "streak.h"
#include "recap.h"
#include "pet_speech.h"
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>
#include <math.h>
#include <esp_random.h>

static_assert(PET_NUM_SPECIES == SPLASH_ANIM_COUNT,
    "PET_NUM_SPECIES must equal SPLASH_ANIM_COUNT — update one when the other changes");

// ============== Internal state ==============
static Pet      g_current = {};
static Gallery  g_gallery = {};

// MetricBaselines (deltas from last BLE push). Persisted to NVS so a
// power-cycle in the middle of a heavy session can't double-count the
// next delta.
struct PetBaselines {
    uint8_t last_s_pct;
    uint8_t last_w_pct;
    uint8_t ses_baseline_count;
    struct { char id[9]; uint8_t c_pct; } ses[SESSION_MAX];
    // Phase 3 — care passive-refill bookkeeping
    uint32_t last_satiety_passive_ts;
    uint8_t  prev_5h_crossed_80;
    uint8_t  prev_weekly_crossed_80;
    uint8_t  prev_session_count;
    uint8_t  prev_compacting_count_seen;
    uint8_t  neglect_event_armed;       // 1 = will fire on next zero-transition
};
static PetBaselines g_baselines = {};

// Phase 3 — manual action cooldown anchors (epoch in pet_now() seconds).
// 0 means "never fired" → cooldown is implicitly elapsed.
static uint32_t g_last_feed_ts = 0;
static uint32_t g_last_play_ts = 0;
static uint32_t g_last_pet_ts  = 0;

// Monotonic clock (spec — "Time source" subsection of section 2).
// pet_now() = pet_now_base + (millis() - pet_now_anchor_ms) / 1000.
// Power-off time does NOT accumulate because pet_now_base is loaded from
// NVS at boot, not from a wall clock.
static uint32_t g_pet_now_base       = 0;
static uint32_t g_pet_now_anchor_ms  = 0;

uint32_t pet_now(void) {
    uint32_t elapsed = (millis() - g_pet_now_anchor_ms) / 1000;
    return g_pet_now_base + elapsed;
}

uint32_t pet_time_since_level_up(void) {
    uint32_t ts = g_current.last_level_up_pet_now;
    // NOTE: returns 0 for both "never recorded" and "level-up happened this very
    // second" — caller treats both the same (history line empty). The edge case
    // is < 1s visible and acceptable.
    if (ts == 0) return 0;
    uint32_t now = pet_now();
    if (now < ts) return 0;  // clock weirdness guard
    return now - ts;
}

// Move the clock base forward so (millis() - anchor) is small. Called from
// flush_nvs and whenever pet_now_base changes. Both halves of pet_now()
// continue to return the same value across this call — no jumps.
static void pet_now_rebase(void) {
    g_pet_now_base       = pet_now();
    g_pet_now_anchor_ms  = millis();
}

// Wall-clock anchor. Updated whenever the daemon pushes a payload with `ts`.
// Zero means "no wall-clock yet" — features depending on this must degrade gracefully.
static uint32_t g_wall_clock_anchor_epoch   = 0;
static uint32_t g_wall_clock_anchor_pet_now = 0;

uint32_t pet_wallclock_now(void) {
    if (g_wall_clock_anchor_epoch == 0) return 0;
    return g_wall_clock_anchor_epoch + (pet_now() - g_wall_clock_anchor_pet_now);
}

void pet_set_wallclock(uint32_t epoch) {
    if (epoch < 1700000000U) return;  // sanity — reject pre-2023 timestamps
    g_wall_clock_anchor_epoch   = epoch;
    g_wall_clock_anchor_pet_now = pet_now();
}

// g_dirty was defined further down inside the NVS section; moved up so it's
// in scope for pet_set_tz_offset (P1-7/P1-8 wiring) without a forward decl.
static bool g_dirty = false;

// Day index = (epoch + tz_offset) / 86400. Local-time matters for streak
// day-boundary feel — midnight UTC is wrong for most users. Loaded from
// NVS on boot so day boundaries are correct even while the daemon is offline.
static int32_t g_tz_offset_sec = 0;   // updated via pet_set_tz_offset
#define NVS_KEY_TZ_OFFSET "tz_o"

void pet_set_tz_offset(int32_t offset_sec) {
    if (offset_sec == g_tz_offset_sec) return;   // no-op; avoid NVS thrash
    g_tz_offset_sec = offset_sec;
    g_dirty = true;                               // picked up by next save_nvs
    Serial.printf("[pet] tz_offset set to %d s (%+d:%02d)\n",
                  (int)offset_sec,
                  (int)(offset_sec / 3600),
                  (int)((offset_sec >= 0 ? offset_sec : -offset_sec) % 3600 / 60));
}

uint32_t pet_today_index(void) {
    uint32_t wc = pet_wallclock_now();
    if (wc == 0) return 0;
    int64_t local = (int64_t)wc + g_tz_offset_sec;
    return (uint32_t)(local / 86400);
}

static uint8_t  g_event_cb_set = 0;
static void (*g_event_cb)(pet_event_t) = nullptr;
// Bitmask of events fired before the callback was registered (bit N = event N pending).
// Replayed in order when pet_set_event_cb() is first called.
static uint32_t g_pending_events = 0;

// (g_dirty was defined here previously — moved above pet_set_tz_offset so it
// stays in scope for early callers without a forward decl.)

// ============== NVS persistence (Arduino Preferences) ==============
#define NVS_NAMESPACE   "pet"
#define NVS_KEY_CURRENT "current"
#define NVS_KEY_GALLERY "gallery"
// Two independent schema versions so the gallery can bump (added the
// graduate `quote` field in V5 for F12) without forcing a no-op
// PersistedCurrent migration at the same time.
#define NVS_CURRENT_V   4
#define NVS_GALLERY_V   5
// Back-compat alias — still used by the few touchpoints that don't
// distinguish (callers being migrated below).
#define NVS_SCHEMA_V    NVS_CURRENT_V

// ============== Achievement counters (task 2.3) — placed early so pet_mark_species_discovered can use them ==============
// Lifetime counters — persisted in dedicated NVS keys so schema bumps are not needed.
#define NVS_KEY_LT_FEED  "lt_f"
#define NVS_KEY_LT_PLAY  "lt_p"
#define NVS_KEY_LT_PET   "lt_pt"
#define NVS_KEY_WEEK_ID  "wk_id"
// Week counters — persisted alongside lifetime counters so a reboot mid-week
// doesn't silently lose quest progress (P1-6 / wave-3-pet-feature-qa).
// Cleared whenever week_rollover_check() observes a new week index.
#define NVS_KEY_WK_FEED  "wk_f"
#define NVS_KEY_WK_PLAY  "wk_p"
#define NVS_KEY_WK_LVUP  "wk_lv"
#define NVS_KEY_WK_DISC  "wk_dc"

static uint32_t g_lifetime_feed = 0;
static uint32_t g_lifetime_play = 0;
static uint32_t g_lifetime_pet  = 0;

// Week counters (persisted — see NVS_KEY_WK_* above).
static uint32_t g_week_id                = 0;   // (pet_today_index() / 7) at last check; 0 means unset
static uint32_t g_week_feed              = 0;
static uint32_t g_week_play              = 0;
static uint32_t g_week_levelups          = 0;
static uint32_t g_week_species_discovered = 0;

// Check for week rollover and reset week counters if the week has advanced.
static void week_rollover_check(void) {
    uint32_t today = pet_today_index();
    if (today == 0) return;          // no wall-clock yet — leave g_week_id at 0
    uint32_t new_week = today / 7;
    if (g_week_id == 0 || new_week != g_week_id) {
        // Week has rolled over (or first initialisation with a valid clock).
        g_week_id                = new_week;
        g_week_feed              = 0;
        g_week_play              = 0;
        g_week_levelups          = 0;
        g_week_species_discovered = 0;
    }
}

// Load lifetime counters from NVS. Called from pet_init().
// Saving is handled by save_nvs() which writes these keys under its existing
// Preferences handle — avoids the ~30ms overhead of a separate handle open
// per care action.
static void load_lifetime_counters(void) {
    Preferences prefs;
    if (prefs.begin(NVS_NAMESPACE, /*readOnly=*/true)) {
        g_lifetime_feed = prefs.getUInt(NVS_KEY_LT_FEED, 0);
        g_lifetime_play = prefs.getUInt(NVS_KEY_LT_PLAY, 0);
        g_lifetime_pet  = prefs.getUInt(NVS_KEY_LT_PET,  0);
        g_week_id                 = prefs.getUInt(NVS_KEY_WEEK_ID, 0);
        g_week_feed               = prefs.getUInt(NVS_KEY_WK_FEED, 0);
        g_week_play               = prefs.getUInt(NVS_KEY_WK_PLAY, 0);
        g_week_levelups           = prefs.getUInt(NVS_KEY_WK_LVUP, 0);
        g_week_species_discovered = prefs.getUInt(NVS_KEY_WK_DISC, 0);
        prefs.end();
    }
    Serial.printf("[pet] lifetime counters: feed=%u play=%u pet=%u\n",
                  (unsigned)g_lifetime_feed, (unsigned)g_lifetime_play, (unsigned)g_lifetime_pet);
    Serial.printf("[pet] week counters: id=%u feed=%u play=%u lvup=%u disc=%u\n",
                  (unsigned)g_week_id, (unsigned)g_week_feed, (unsigned)g_week_play,
                  (unsigned)g_week_levelups, (unsigned)g_week_species_discovered);
}

uint32_t pet_lifetime_feed_count(void)          { return g_lifetime_feed; }
uint32_t pet_lifetime_play_count(void)          { return g_lifetime_play; }
uint32_t pet_lifetime_pet_count(void)           { return g_lifetime_pet; }
uint32_t pet_week_feed_count(void)              { return g_week_feed; }
uint32_t pet_week_play_count(void)              { return g_week_play; }
uint32_t pet_week_levelups(void)                { return g_week_levelups; }
uint32_t pet_week_species_discovered(void)      { return g_week_species_discovered; }

// Species catalog discovery bitmasks — additive NVS keys (no schema bump needed).
#define NVS_KEY_DISCOVERED_MASK  "disc_m"   // uint16_t — bit N = species N ever owned
#define NVS_KEY_DISCOVERED_SHINY "disc_s"   // uint16_t — bit N = shiny variant of species N seen

static uint16_t g_discovered_mask       = 0;
static uint16_t g_discovered_shiny_mask = 0;

uint16_t pet_discovered_species_mask(void) { return g_discovered_mask; }
uint16_t pet_discovered_shiny_mask(void)   { return g_discovered_shiny_mask; }

void pet_mark_species_discovered(uint16_t species_id, bool shiny) {
    if (species_id >= PET_NUM_SPECIES) return;
    uint16_t bit = (uint16_t)(1u << species_id);
    bool changed = false;
    if (!(g_discovered_mask & bit)) {
        g_discovered_mask |= bit;
        changed = true;
        // Achievement week counter — only on brand-new regular discovery (task 2.3).
        week_rollover_check();
        g_week_species_discovered++;
    }
    if (shiny && !(g_discovered_shiny_mask & bit)) {
        g_discovered_shiny_mask |= bit;
        changed = true;
        recap_note_shiny_discovered();
    }
    if (changed) {
        g_dirty = true;   // discovery masks persisted by next save_nvs() cadence
    }
}

uint8_t pet_discovered_count(void) {
    return (uint8_t)__builtin_popcount(g_discovered_mask);
}
uint8_t pet_discovered_shiny_count(void) {
    return (uint8_t)__builtin_popcount(g_discovered_shiny_mask);
}

// ============== Vacation mode — module-level, dedicated NVS keys ==============
// Not part of Pet struct (vacation is a device/account setting, not a pet
// attribute). Persisted under separate keys so NVS_SCHEMA_V does not need
// to be bumped — Preferences adds keys without altering the existing blob.
#define NVS_KEY_VACATION_ACTIVE   "vac_a"   // uint8_t, 0/1
#define NVS_KEY_VACATION_STARTED  "vac_s"   // uint32_t, pet_now() at activation
// Wall-clock anchor (epoch seconds) at activation. Used to honor the 14-day
// cap across power-off intervals — pet_now() is monotonic and doesn't count
// time the device was unplugged (P1-4). Defaults to 0 when no daemon clock
// was available at activation; we fall back to the monotonic path then.
#define NVS_KEY_VACATION_WALLCLOCK "vac_w"
#define VACATION_MAX_DAYS         14

static bool     g_vacation_active            = false;
static uint32_t g_vacation_started_pet_now   = 0;
static uint32_t g_vacation_started_wallclock = 0;

bool pet_is_vacation_active(void) {
    return g_vacation_active;
}

uint32_t pet_vacation_days_remaining(void) {
    if (!g_vacation_active) return 0;
    uint32_t elapsed;
    uint32_t wc_now = pet_wallclock_now();
    if (wc_now > 0 && g_vacation_started_wallclock > 0) {
        // Wall-clock available — use it so the countdown matches user-perceived
        // calendar time even across power-off (P1-4).
        elapsed = (wc_now > g_vacation_started_wallclock)
                    ? wc_now - g_vacation_started_wallclock : 0;
    } else {
        // Fallback: monotonic. Loses power-off time but is the only option
        // when no daemon ever pushed wallclock.
        elapsed = pet_now() - g_vacation_started_pet_now;
    }
    uint32_t total_secs = (uint32_t)VACATION_MAX_DAYS * 86400u;
    if (elapsed >= total_secs) return 0;
    uint32_t remaining_secs = total_secs - elapsed;
    return (remaining_secs + 86399u) / 86400u;  // ceiling: 1d remaining shows "1" not "0"
}

void pet_set_vacation(bool active) {
    bool was_active                  = g_vacation_active;
    g_vacation_active                = active;
    g_vacation_started_pet_now       = active ? pet_now() : 0;
    g_vacation_started_wallclock     = active ? pet_wallclock_now() : 0;
    Preferences prefs;
    if (prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
        prefs.putUChar(NVS_KEY_VACATION_ACTIVE,    (uint8_t)(active ? 1 : 0));
        prefs.putUInt (NVS_KEY_VACATION_STARTED,   g_vacation_started_pet_now);
        prefs.putUInt (NVS_KEY_VACATION_WALLCLOCK, g_vacation_started_wallclock);
        prefs.end();
    }
    Serial.printf("[pet] vacation %s\n", active ? "ON" : "OFF");

    // Resume-day credit (P1-3): when vacation transitions off, grant the
    // current day a streak credit so the next rollover doesn't reset the
    // streak just because the user happened to come back without caring
    // that exact day. streak_note_* sets the per-day active flag and
    // persists it via the streak module's own NVS namespace — survives
    // a reboot between toggle-off and the next rollover.
    if (was_active && !active) {
        streak_note_care_action();
        streak_note_claude_session();
    }
}

void pet_vacation_start(uint32_t days) {
    if (days == 0) days = 1;
    if (days > VACATION_MAX_DAYS) days = VACATION_MAX_DAYS;
    // Backdate the start so days_remaining math yields `days`.
    uint32_t backdate_secs = ((uint32_t)VACATION_MAX_DAYS - days) * 86400u;
    g_vacation_active            = true;
    uint32_t now_pn = pet_now();
    uint32_t now_wc = pet_wallclock_now();
    g_vacation_started_pet_now   = (now_pn   > backdate_secs) ? now_pn   - backdate_secs : 0;
    g_vacation_started_wallclock = (now_wc   > backdate_secs) ? now_wc   - backdate_secs : 0;
    Preferences prefs;
    if (prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
        prefs.putUChar(NVS_KEY_VACATION_ACTIVE,    1);
        prefs.putUInt (NVS_KEY_VACATION_STARTED,   g_vacation_started_pet_now);
        prefs.putUInt (NVS_KEY_VACATION_WALLCLOCK, g_vacation_started_wallclock);
        prefs.end();
    }
    Serial.printf("[pet] vacation ON (%u days remaining via sim)\n", (unsigned)days);
}

// Frozen v1 layout — used ONLY for the v1 → v2 migration path in load_nvs.
// Do not modify; any future schema change adds its own frozen prior layout
// alongside this one. Keep the field order identical to the original v1
// PersistedCurrent definition.
struct PersistedCurrentV1 {
    uint16_t version;
    uint16_t species_id;
    uint8_t  level;
    uint32_t xp;
    uint32_t born_ts;
    uint32_t last_event_ts;
    uint32_t pet_now_base;
    uint8_t  last_s_pct;
    uint8_t  last_w_pct;
    uint8_t  ses_baseline_count;
    struct { char id[9]; uint8_t c_pct; } ses[SESSION_MAX];
};

// Frozen v3 layout — used ONLY for the v3 → v4 migration path in load_nvs.
// Do not modify. Added alongside V4 additions (name + shiny fields).
struct PersistedCurrentV3 {
    uint16_t version;
    uint16_t species_id;
    uint8_t  level;
    uint32_t xp;
    uint32_t born_ts;
    uint32_t last_event_ts;
    uint32_t pet_now_base;
    uint8_t  last_s_pct;
    uint8_t  last_w_pct;
    uint8_t  ses_baseline_count;
    struct { char id[9]; uint8_t c_pct; } ses[SESSION_MAX];
    // Phase 3 — care stats
    uint8_t  satiety;
    uint8_t  spirit;
    uint8_t  bond;
    uint32_t satiety_decay_ts;
    uint32_t spirit_decay_ts;
    uint32_t bond_decay_ts;
    uint32_t last_feed_ts;
    uint32_t last_play_ts;
    uint32_t last_pet_ts;
    // Phase 3 — passive-refill baselines (mirror of PetBaselines tail)
    uint32_t last_satiety_passive_ts;
    uint8_t  prev_5h_crossed_80;
    uint8_t  prev_weekly_crossed_80;
    uint8_t  prev_session_count;
    uint8_t  prev_compacting_count_seen;
    uint8_t  neglect_event_armed;
};

// Current (V4) layout — adds name and shiny to PersistedCurrentV3.
struct PersistedCurrent {
    uint16_t version;          // 4
    uint16_t species_id;
    uint8_t  level;
    uint32_t xp;
    uint32_t born_ts;
    uint32_t last_event_ts;
    uint32_t pet_now_base;
    uint8_t  last_s_pct;
    uint8_t  last_w_pct;
    uint8_t  ses_baseline_count;
    struct { char id[9]; uint8_t c_pct; } ses[SESSION_MAX];
    // Phase 3 — care stats
    uint8_t  satiety;
    uint8_t  spirit;
    uint8_t  bond;
    uint32_t satiety_decay_ts;
    uint32_t spirit_decay_ts;
    uint32_t bond_decay_ts;
    uint32_t last_feed_ts;
    uint32_t last_play_ts;
    uint32_t last_pet_ts;
    // Phase 3 — passive-refill baselines (mirror of PetBaselines tail)
    uint32_t last_satiety_passive_ts;
    uint8_t  prev_5h_crossed_80;
    uint8_t  prev_weekly_crossed_80;
    uint8_t  prev_session_count;
    uint8_t  prev_compacting_count_seen;
    uint8_t  neglect_event_armed;
    // V4 additions:
    char     name[16];
    uint8_t  is_shiny;
    uint8_t  _pad[2];
};

// Safety guards: all persisted-struct sizes must remain distinct so the
// size-dispatched migration in load_nvs() routes correctly.
static_assert(sizeof(PersistedCurrentV1) != sizeof(PersistedCurrentV3),
              "PersistedCurrentV1 and V3 must differ in size for size-dispatch migration");
static_assert(sizeof(PersistedCurrentV3) != sizeof(PersistedCurrent),
              "PersistedCurrentV3 and V4 must differ in size for size-dispatch migration");
static_assert(sizeof(PersistedCurrentV1) != sizeof(PersistedCurrent),
              "PersistedCurrentV1 and V4 must differ in size for size-dispatch migration");

// Frozen Graduate layout used by PersistedGalleryV3 — matches the Graduate
// struct BEFORE the V4 name + shiny additions.
struct GraduateV3 {
    uint16_t species_id;
    uint32_t born_ts;
    uint32_t graduated_ts;
    uint32_t total_xp_earned;
};

// Frozen v3 gallery layout — Graduate entries use the old (pre-V4) Graduate size.
struct PersistedGalleryV3 {
    uint16_t version;
    uint16_t count;
    GraduateV3 entries[PET_GALLERY_CAP];
};

// Frozen Graduate layout used by PersistedGalleryV4 — matches Graduate
// BEFORE the V5 quote field.
struct GraduateV4 {
    uint16_t species_id;
    uint32_t born_ts;
    uint32_t graduated_ts;
    uint32_t total_xp_earned;
    char     name[16];
    uint8_t  is_shiny;
    uint8_t  _pad[3];
};

// Frozen v4 gallery layout.
struct PersistedGalleryV4 {
    uint16_t   version;
    uint16_t   count;
    GraduateV4 entries[PET_GALLERY_CAP];
};

// Current (V5) gallery — Graduate entries now include the F12 quote field.
struct PersistedGallery {
    uint16_t version;
    uint16_t count;
    Graduate entries[PET_GALLERY_CAP];
};

static_assert(sizeof(PersistedGalleryV3) != sizeof(PersistedGalleryV4),
              "PersistedGalleryV3 and V4 must differ in size for size-dispatch migration");
static_assert(sizeof(PersistedGalleryV4) != sizeof(PersistedGallery),
              "PersistedGalleryV4 and V5 must differ in size for size-dispatch migration");
static_assert(sizeof(PersistedGalleryV3) != sizeof(PersistedGallery),
              "PersistedGalleryV3 and V5 must differ in size for size-dispatch migration");

static void save_nvs(void);   // forward decl — load_nvs calls it on v1 → v2 migration

// Helper: copy a GraduateV3 (pre-V4, no name/shiny) into a Graduate (current),
// defaulting the new V4 fields from the species name + V5 quote to empty.
static void migrate_graduate_v3_to_current(Graduate* dst, const GraduateV3* src) {
    dst->species_id      = src->species_id;
    dst->born_ts         = src->born_ts;
    dst->graduated_ts    = src->graduated_ts;
    dst->total_xp_earned = src->total_xp_earned;
    strncpy(dst->name, pet_species_name(src->species_id), sizeof(dst->name) - 1);
    dst->name[sizeof(dst->name) - 1] = '\0';
    dst->is_shiny = 0;
    dst->_pad[0] = 0;
    dst->_pad[1] = 0;
    dst->_pad[2] = 0;
    dst->quote[0] = '\0';
}

// Compat alias for any caller still referencing the old name.
#define migrate_graduate_v3_to_v4 migrate_graduate_v3_to_current

// V4 (no quote) -> V5 (with quote): copy verbatim, zero the quote field.
static void migrate_graduate_v4_to_v5(Graduate* dst, const GraduateV4* src) {
    dst->species_id      = src->species_id;
    dst->born_ts         = src->born_ts;
    dst->graduated_ts    = src->graduated_ts;
    dst->total_xp_earned = src->total_xp_earned;
    memcpy(dst->name, src->name, sizeof(dst->name));
    dst->name[sizeof(dst->name) - 1] = '\0';
    dst->is_shiny = src->is_shiny;
    dst->_pad[0] = 0;
    dst->_pad[1] = 0;
    dst->_pad[2] = 0;
    dst->quote[0] = '\0';
}

// Try to load existing state from NVS. Returns true on success.
// Handles V1 (phase 1/2), V2/V3 (phase 3+), and V4 (current) records.
// Older records are migrated in-place by defaulting new fields and
// re-persisting as V4. Each migration branch is self-contained: it reads
// the gallery under its own nested Preferences handle, ends() the main
// handle, then calls save_nvs() and returns true. This avoids the race
// where save_nvs() would clobber a gallery we haven't read yet.
static bool load_nvs(void) {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/true)) return false;

    size_t stored_size = prefs.getBytesLength(NVS_KEY_CURRENT);

    if (stored_size == sizeof(PersistedCurrentV1)) {
        // Legacy v1 record — read into the frozen layout and migrate.
        PersistedCurrentV1 pcv1 = {};
        prefs.getBytes(NVS_KEY_CURRENT, &pcv1, sizeof(pcv1));
        if (pcv1.version != 1) {
            prefs.end();
            return false;
        }

        g_current.species_id    = pcv1.species_id;
        if (g_current.species_id >= PET_NUM_SPECIES) g_current.species_id = 0;
        g_current.level         = pcv1.level;
        g_current.xp            = pcv1.xp;
        g_current.born_ts       = pcv1.born_ts;
        g_current.last_event_ts = pcv1.last_event_ts;
        g_pet_now_base          = pcv1.pet_now_base;
        g_pet_now_anchor_ms     = millis();
        g_baselines.last_s_pct  = pcv1.last_s_pct;
        g_baselines.last_w_pct  = pcv1.last_w_pct;
        g_baselines.ses_baseline_count = pcv1.ses_baseline_count;
        memcpy(g_baselines.ses, pcv1.ses, sizeof(g_baselines.ses));

        // Default phase-3 fields. Stats start at 80 so an upgrading user
        // doesn't see a starving pet; decay clocks seed to pet_now() so
        // decay starts fresh; cooldowns at 0 (fully ready).
        g_current.satiety = 80;
        g_current.spirit  = 80;
        g_current.bond    = 80;
        uint32_t now = pet_now();
        g_current.satiety_decay_ts = now;
        g_current.spirit_decay_ts  = now;
        g_current.bond_decay_ts    = now;
        g_last_feed_ts = 0;
        g_last_play_ts = 0;
        g_last_pet_ts  = 0;
        g_baselines.last_satiety_passive_ts = 0;
        g_baselines.prev_5h_crossed_80 = 0;
        g_baselines.prev_weekly_crossed_80 = 0;
        g_baselines.prev_session_count = 0;
        g_baselines.prev_compacting_count_seen = 0;
        g_baselines.neglect_event_armed = 1;
        // V4 defaults — name from species, not shiny.
        strncpy(g_current.name, pet_species_name(g_current.species_id), sizeof(g_current.name) - 1);
        g_current.name[sizeof(g_current.name) - 1] = '\0';
        g_current.is_shiny = 0;

        // v1 gallery: same Graduate shape as V3 (pre-V4). Read into frozen
        // PersistedGalleryV3, then migrate each entry to V4 Graduate.
        PersistedGalleryV3 pgv1 = {};
        size_t gv1_got = prefs.getBytes(NVS_KEY_GALLERY, &pgv1, sizeof(pgv1));
        if (gv1_got == sizeof(pgv1) && pgv1.version == 1) {
            g_gallery.count = pgv1.count;
            if (g_gallery.count > PET_GALLERY_CAP) g_gallery.count = PET_GALLERY_CAP;
            for (uint16_t i = 0; i < g_gallery.count; i++) {
                migrate_graduate_v3_to_v4(&g_gallery.entries[i], &pgv1.entries[i]);
            }
            Serial.printf("pet: migrated v1 gallery (%u graduates)\n",
                          (unsigned)g_gallery.count);
        } else {
            g_gallery.count = 0;
        }

        prefs.end();
        Serial.println("pet: migrated v1 NVS → v4 (stats default to 80, name/shiny defaulted)");
        save_nvs();   // immediately re-persist as v4 (current + gallery)
        return true;  // v1 branch handles its own gallery — skip shared block

    } else if (stored_size == sizeof(PersistedCurrentV3)) {
        // V2 or V3 record (both share the same PersistedCurrentV3 layout).
        PersistedCurrentV3 pc3 = {};
        prefs.getBytes(NVS_KEY_CURRENT, &pc3, sizeof(pc3));

        if (pc3.version != 2 && pc3.version != 3) {
            prefs.end();
            return false;
        }
        // Both V2 and V3 on-disk layouts are identical — migrate straight to V4.
        Serial.printf("[pet] migrated V%u -> V4\n", pc3.version);
        prefs.end();
        // Why this branch is self-contained instead of falling through to
        // the shared load path below: (1) save_nvs() opens its own
        // Preferences handle, so we must end() the current one first;
        // (2) the shared gallery block at the bottom of load_nvs checks
        // pg.version == NVS_SCHEMA_V and would silently zero g_gallery
        // if it saw an old gallery on disk — so the gallery must be read
        // here under a nested handle before save_nvs() re-stamps it.
        // Same pattern applies to any future VN -> VN+1 migration.

        // Populate g_current and g_baselines from the V3 record.
        g_current.species_id    = pc3.species_id;
        if (g_current.species_id >= PET_NUM_SPECIES) g_current.species_id = 0;
        g_current.level         = pc3.level;
        g_current.xp            = pc3.xp;
        g_current.born_ts       = pc3.born_ts;
        g_current.last_event_ts = pc3.last_event_ts;
        g_pet_now_base          = pc3.pet_now_base;
        g_pet_now_anchor_ms     = millis();
        g_baselines.last_s_pct  = pc3.last_s_pct;
        g_baselines.last_w_pct  = pc3.last_w_pct;
        g_baselines.ses_baseline_count = pc3.ses_baseline_count;
        memcpy(g_baselines.ses, pc3.ses, sizeof(g_baselines.ses));
        g_current.satiety = pc3.satiety;
        g_current.spirit  = pc3.spirit;
        g_current.bond    = pc3.bond;
        g_current.satiety_decay_ts = pc3.satiety_decay_ts;
        g_current.spirit_decay_ts  = pc3.spirit_decay_ts;
        g_current.bond_decay_ts    = pc3.bond_decay_ts;
        g_last_feed_ts = pc3.last_feed_ts;
        g_last_play_ts = pc3.last_play_ts;
        g_last_pet_ts  = pc3.last_pet_ts;
        g_baselines.last_satiety_passive_ts = pc3.last_satiety_passive_ts;
        g_baselines.prev_5h_crossed_80 = pc3.prev_5h_crossed_80;
        g_baselines.prev_weekly_crossed_80 = pc3.prev_weekly_crossed_80;
        g_baselines.prev_session_count = pc3.prev_session_count;
        g_baselines.prev_compacting_count_seen = pc3.prev_compacting_count_seen;
        g_baselines.neglect_event_armed = pc3.neglect_event_armed;
        // V4 defaults — name from species, not shiny.
        strncpy(g_current.name, pet_species_name(g_current.species_id), sizeof(g_current.name) - 1);
        g_current.name[sizeof(g_current.name) - 1] = '\0';
        g_current.is_shiny = 0;

        // Gallery: read under a nested handle. V2 and V3 galleries both
        // use the old Graduate size (PersistedGalleryV3), so check both
        // version values and migrate each entry to V4.
        {
            Preferences gprefs;
            if (gprefs.begin(NVS_NAMESPACE, /*readOnly=*/true)) {
                PersistedGalleryV3 pgv3 = {};
                size_t ggot = gprefs.getBytes(NVS_KEY_GALLERY, &pgv3, sizeof(pgv3));
                if (ggot == sizeof(pgv3) && (pgv3.version == 2 || pgv3.version == 3)) {
                    g_gallery.count = pgv3.count;
                    if (g_gallery.count > PET_GALLERY_CAP) g_gallery.count = PET_GALLERY_CAP;
                    for (uint16_t i = 0; i < g_gallery.count; i++) {
                        migrate_graduate_v3_to_v4(&g_gallery.entries[i], &pgv3.entries[i]);
                    }
                    Serial.printf("[pet] migrated V%u gallery (%u graduates)\n",
                                  pgv3.version, (unsigned)g_gallery.count);
                }
                gprefs.end();
            }
        }
        save_nvs();   // re-persists both current (V4) and gallery (V4)
        return true;

    } else if (stored_size == sizeof(PersistedCurrent)) {
        // Native V4 record.
        PersistedCurrent pc = {};
        prefs.getBytes(NVS_KEY_CURRENT, &pc, sizeof(pc));

        if (pc.version != NVS_SCHEMA_V) {
            prefs.end();
            return false;
        }

        g_current.species_id    = pc.species_id;
        if (g_current.species_id >= PET_NUM_SPECIES) g_current.species_id = 0;
        g_current.level         = pc.level;
        g_current.xp            = pc.xp;
        g_current.born_ts       = pc.born_ts;
        g_current.last_event_ts = pc.last_event_ts;
        g_pet_now_base          = pc.pet_now_base;
        g_pet_now_anchor_ms     = millis();
        g_baselines.last_s_pct  = pc.last_s_pct;
        g_baselines.last_w_pct  = pc.last_w_pct;
        g_baselines.ses_baseline_count = pc.ses_baseline_count;
        memcpy(g_baselines.ses, pc.ses, sizeof(g_baselines.ses));
        g_current.satiety = pc.satiety;
        g_current.spirit  = pc.spirit;
        g_current.bond    = pc.bond;
        g_current.satiety_decay_ts = pc.satiety_decay_ts;
        g_current.spirit_decay_ts  = pc.spirit_decay_ts;
        g_current.bond_decay_ts    = pc.bond_decay_ts;
        g_last_feed_ts = pc.last_feed_ts;
        g_last_play_ts = pc.last_play_ts;
        g_last_pet_ts  = pc.last_pet_ts;
        g_baselines.last_satiety_passive_ts = pc.last_satiety_passive_ts;
        g_baselines.prev_5h_crossed_80 = pc.prev_5h_crossed_80;
        g_baselines.prev_weekly_crossed_80 = pc.prev_weekly_crossed_80;
        g_baselines.prev_session_count = pc.prev_session_count;
        g_baselines.prev_compacting_count_seen = pc.prev_compacting_count_seen;
        g_baselines.neglect_event_armed = pc.neglect_event_armed;
        // V4 fields:
        memcpy(g_current.name, pc.name, sizeof(g_current.name));
        g_current.name[sizeof(g_current.name) - 1] = '\0';   // paranoid null-term
        g_current.is_shiny = pc.is_shiny;
    } else {
        // Unknown size — fresh boot.
        prefs.end();
        return false;
    }

    // Gallery is optional — absence is not a failure; just means no graduates yet.
    // Size-dispatch on the stored bytes so a pre-V5 gallery (no quote field)
    // migrates in place rather than getting zeroed.
    size_t ggot_len = prefs.getBytesLength(NVS_KEY_GALLERY);
    g_gallery.count = 0;
    if (ggot_len == sizeof(PersistedGallery)) {
        PersistedGallery pg = {};
        prefs.getBytes(NVS_KEY_GALLERY, &pg, sizeof(pg));
        if (pg.version == NVS_GALLERY_V) {
            g_gallery.count = pg.count;
            if (g_gallery.count > PET_GALLERY_CAP) g_gallery.count = PET_GALLERY_CAP;
            memcpy(g_gallery.entries, pg.entries, sizeof(g_gallery.entries));
        }
    } else if (ggot_len == sizeof(PersistedGalleryV4)) {
        // V4 → V5 migration: copy entries, leave quote empty. Re-stamp on
        // next save_nvs cadence (g_dirty will fire from the migration log).
        PersistedGalleryV4 pgv4 = {};
        prefs.getBytes(NVS_KEY_GALLERY, &pgv4, sizeof(pgv4));
        if (pgv4.version == 4) {
            g_gallery.count = pgv4.count;
            if (g_gallery.count > PET_GALLERY_CAP) g_gallery.count = PET_GALLERY_CAP;
            for (uint16_t i = 0; i < g_gallery.count; i++) {
                migrate_graduate_v4_to_v5(&g_gallery.entries[i], &pgv4.entries[i]);
            }
            Serial.printf("[pet] migrated V4 gallery (%u graduates) — empty quotes\n",
                          (unsigned)g_gallery.count);
            g_dirty = true;
        }
    } else if (ggot_len == sizeof(PersistedGalleryV3)) {
        // V3 gallery seen at this shared-block read path (very old install
        // taking the V4-native current branch); migrate in place.
        PersistedGalleryV3 pgv3 = {};
        prefs.getBytes(NVS_KEY_GALLERY, &pgv3, sizeof(pgv3));
        if (pgv3.version == 2 || pgv3.version == 3) {
            g_gallery.count = pgv3.count;
            if (g_gallery.count > PET_GALLERY_CAP) g_gallery.count = PET_GALLERY_CAP;
            for (uint16_t i = 0; i < g_gallery.count; i++) {
                migrate_graduate_v3_to_current(&g_gallery.entries[i], &pgv3.entries[i]);
            }
            Serial.printf("[pet] migrated V%u gallery (%u graduates) — empty quotes\n",
                          pgv3.version, (unsigned)g_gallery.count);
            g_dirty = true;
        }
    }

    // Discovery masks — additive keys, loaded here inside the already-open
    // handle so pet_init() doesn't need to open a second handle for them.
    g_discovered_mask       = prefs.getUShort(NVS_KEY_DISCOVERED_MASK,  0);
    g_discovered_shiny_mask = prefs.getUShort(NVS_KEY_DISCOVERED_SHINY, 0);

    // Vacation keys are also additive — load them here while the handle is
    // still open rather than under a separate handle.
    g_vacation_active            = prefs.getUChar(NVS_KEY_VACATION_ACTIVE,    0) != 0;
    g_vacation_started_pet_now   = prefs.getUInt (NVS_KEY_VACATION_STARTED,   0);
    g_vacation_started_wallclock = prefs.getUInt (NVS_KEY_VACATION_WALLCLOCK, 0);

    // TZ offset (P1-7/P1-8) — additive key. Persisted so day boundaries are
    // correct on boot before any daemon push arrives. Read as int (signed).
    g_tz_offset_sec = (int32_t)prefs.getInt(NVS_KEY_TZ_OFFSET, 0);

    prefs.end();

    if (g_vacation_active) {
        Serial.printf("[pet] vacation loaded: started=%lu remaining=%lud\n",
            (unsigned long)g_vacation_started_pet_now,
            (unsigned long)pet_vacation_days_remaining());
    }

    return true;
}

// Write current state to NVS. Called from flush_nvs() (cadence in pet_tick).
static void save_nvs(void) {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
        Serial.println("pet: NVS begin failed; skipping save");
        return;
    }

    // Snapshot the clock into the persisted base, then re-anchor so the
    // in-RAM clock continues seamlessly.
    pet_now_rebase();

    PersistedCurrent pc = {};
    pc.version             = NVS_SCHEMA_V;
    pc.species_id          = g_current.species_id;
    pc.level               = g_current.level;
    pc.xp                  = g_current.xp;
    pc.born_ts             = g_current.born_ts;
    pc.last_event_ts       = g_current.last_event_ts;
    pc.pet_now_base        = g_pet_now_base;
    pc.last_s_pct          = g_baselines.last_s_pct;
    pc.last_w_pct          = g_baselines.last_w_pct;
    pc.ses_baseline_count  = g_baselines.ses_baseline_count;
    memcpy(pc.ses, g_baselines.ses, sizeof(pc.ses));
    // Phase 3 — care state
    pc.satiety              = g_current.satiety;
    pc.spirit               = g_current.spirit;
    pc.bond                 = g_current.bond;
    pc.satiety_decay_ts     = g_current.satiety_decay_ts;
    pc.spirit_decay_ts      = g_current.spirit_decay_ts;
    pc.bond_decay_ts        = g_current.bond_decay_ts;
    pc.last_feed_ts         = g_last_feed_ts;
    pc.last_play_ts         = g_last_play_ts;
    pc.last_pet_ts          = g_last_pet_ts;
    pc.last_satiety_passive_ts   = g_baselines.last_satiety_passive_ts;
    pc.prev_5h_crossed_80        = g_baselines.prev_5h_crossed_80;
    pc.prev_weekly_crossed_80    = g_baselines.prev_weekly_crossed_80;
    pc.prev_session_count        = g_baselines.prev_session_count;
    pc.prev_compacting_count_seen = g_baselines.prev_compacting_count_seen;
    pc.neglect_event_armed       = g_baselines.neglect_event_armed;
    // V4 fields:
    memcpy(pc.name, g_current.name, sizeof(pc.name));
    pc.is_shiny                  = g_current.is_shiny;
    prefs.putBytes(NVS_KEY_CURRENT, &pc, sizeof(pc));

    // PersistedGallery is ~5 KB (PET_GALLERY_CAP × Graduate). Allocating it on
    // the stack here overflowed the 8 KB loopTask stack whenever save_nvs() was
    // reached through the deep LVGL keyboard-event chain (the pet-rename path:
    // indev → buttonmatrix → keyboard → textarea READY → on_ta_ready →
    // pet_rename → save_nvs). The stack-canary watchpoint panicked before
    // prefs.end() committed, so the new name was silently lost and the device
    // rebooted. Stage the big struct on the heap so this path is depth-safe.
    PersistedGallery* pg = (PersistedGallery*)calloc(1, sizeof(PersistedGallery));
    if (pg) {
        pg->version = NVS_GALLERY_V;   // V5 — Graduate now carries quote[64]
        pg->count   = g_gallery.count;
        memcpy(pg->entries, g_gallery.entries, sizeof(pg->entries));
        prefs.putBytes(NVS_KEY_GALLERY, pg, sizeof(*pg));
        free(pg);
    } else {
        Serial.println("pet: save_nvs gallery alloc failed; skipping gallery flush");
    }

    // Lifetime counters — additive keys, written here so a single save_nvs()
    // call covers them without a separate Preferences handle per care action.
    prefs.putUInt(NVS_KEY_LT_FEED, g_lifetime_feed);
    prefs.putUInt(NVS_KEY_LT_PLAY, g_lifetime_play);
    prefs.putUInt(NVS_KEY_LT_PET,  g_lifetime_pet);

    // Week counters — additive keys (P1-6). Persisting these means a reboot
    // mid-week preserves the user's quest progress; previously a reboot reset
    // them to 0 silently. Cleared by week_rollover_check() on week change.
    prefs.putUInt(NVS_KEY_WEEK_ID, g_week_id);
    prefs.putUInt(NVS_KEY_WK_FEED, g_week_feed);
    prefs.putUInt(NVS_KEY_WK_PLAY, g_week_play);
    prefs.putUInt(NVS_KEY_WK_LVUP, g_week_levelups);
    prefs.putUInt(NVS_KEY_WK_DISC, g_week_species_discovered);

    // TZ offset (P1-7/P1-8) — additive signed key. Survives daemon outage.
    prefs.putInt(NVS_KEY_TZ_OFFSET, (int32_t)g_tz_offset_sec);

    // Discovery masks — deferred here from pet_mark_species_discovered() so
    // individual discoveries don't open their own Preferences handles and stall
    // the main loop. Worst case (power-off before the next flush): user re-plays
    // the discovery on next hatch. Acceptable — discovery is always replayable.
    prefs.putUShort(NVS_KEY_DISCOVERED_MASK,  g_discovered_mask);
    prefs.putUShort(NVS_KEY_DISCOVERED_SHINY, g_discovered_shiny_mask);

    prefs.end();
}

static void fire_event(pet_event_t ev);   // forward decl — needed by instantiate_pet and pet_tick

// Spawn a fresh L1 pet of the given species. Stamps timestamps from pet_now().
// Fires PET_EVENT_HATCHED after the new pet is fully initialised. If the event
// callback has not been registered yet (e.g. on first boot before ui_init()
// completes), the event is queued for replay when pet_set_event_cb() is called.
static void instantiate_pet(uint16_t species_id) {
    g_current.species_id    = species_id;
    g_current.level         = 1;
    g_current.xp            = 0;
    g_current.born_ts       = pet_now();
    g_current.last_event_ts = g_current.born_ts;
    // Phase 3 — fresh pet starts healthy. Decay clocks anchored at born_ts.
    g_current.satiety       = 80;
    g_current.spirit        = 80;
    g_current.bond          = 80;
    g_current.satiety_decay_ts = g_current.born_ts;
    g_current.spirit_decay_ts  = g_current.born_ts;
    g_current.bond_decay_ts    = g_current.born_ts;
    g_last_feed_ts = 0;
    g_last_play_ts = 0;
    g_last_pet_ts  = 0;
    g_baselines = {};        // fresh baselines — first metrics push will seed them
    g_baselines.neglect_event_armed = 1;
    // V4 fields — default name to species name; 1/64 chance of shiny.
    strncpy(g_current.name, pet_species_name(species_id), sizeof(g_current.name) - 1);
    g_current.name[sizeof(g_current.name) - 1] = '\0';
    g_current.is_shiny = ((esp_random() & 0x3F) == 0) ? 1 : 0;

    Serial.printf("pet: HATCHED species=%u name=%s shiny=%u\n",
                  species_id, g_current.name, (unsigned)g_current.is_shiny);
    pet_mark_species_discovered(g_current.species_id, g_current.is_shiny != 0);
    // recap_note_shiny_discovered is called inside pet_mark_species_discovered
    // when the shiny bit is newly set, so no duplicate call here.
    fire_event(PET_EVENT_HATCHED);
}

// ============== Public API ==============
void pet_init(void) {
    g_pet_now_anchor_ms = millis();
    if (load_nvs()) {
        Serial.printf("pet: loaded L%u species=%u xp=%lu born=%lu (now=%lu)\n",
            g_current.level, g_current.species_id,
            (unsigned long)g_current.xp,
            (unsigned long)g_current.born_ts,
            (unsigned long)pet_now());
    } else {
        Serial.println("pet: no saved state — spawning fresh L1 pet (species 0)");
        instantiate_pet(0);
        save_nvs();
    }

    // Backfill: current pet + gallery may pre-date discovery tracking; seed masks now.
    // (Discovery masks are loaded inside load_nvs() for the V4 native path; for
    // migration paths they default to 0 here and are then seeded by the loop below.)
    pet_mark_species_discovered(g_current.species_id, g_current.is_shiny != 0);
    for (uint16_t i = 0; i < g_gallery.count; i++) {
        pet_mark_species_discovered(g_gallery.entries[i].species_id,
                                    g_gallery.entries[i].is_shiny != 0);
    }

    // Load achievement counters (task 2.3).
    load_lifetime_counters();
}

// Wave 4: decay forgiveness. When the pet has had no XP event for
// FORGIVENESS_TRIGGER_NO_USAGE_SEC seconds, decay runs at 1/slowdown_factor rate.
// Uses pet_now() for monotonic comparison; last_event_ts is rebased on every
// grant_xp call.
static uint32_t effective_decay_interval(uint32_t now) {
    uint32_t last = g_current.last_event_ts;
    if (last == 0 || now <= last) return STAT_DECAY_INTERVAL;
    uint32_t idle = now - last;
    if (idle >= (uint32_t)FORGIVENESS_TRIGGER_NO_USAGE_SEC) {
        // Log once per idle-floor crossing so hardware verify can observe the
        // transition without flooding the serial console.
        static uint32_t s_last_logged_idle_floor = 0;
        uint32_t floor = idle / FORGIVENESS_TRIGGER_NO_USAGE_SEC;
        if (floor != s_last_logged_idle_floor) {
            s_last_logged_idle_floor = floor;
            Serial.printf("[pet] forgiveness: idle=%lu sec → decay slowed x%d\n",
                          (unsigned long)idle, FORGIVENESS_SLOWDOWN_FACTOR);
        }
        return STAT_DECAY_INTERVAL * (uint32_t)FORGIVENESS_SLOWDOWN_FACTOR;
    }
    return STAT_DECAY_INTERVAL;
}

// ============== Tick ==============
//
// P2-3 contract: pet_tick MUST stay free of LVGL writes. Decay, neglect-event
// firing, and NVS flush all need to run regardless of which screen is active
// (so a user who never opens SCREEN_PET still has stats that reflect elapsed
// time). Rendering is split out into pet_screen_tick() (gated by
// ui_tick_anim's `if (current_screen == SCREEN_PET)` early-return) and into
// pet_screen_on_event() (which itself bails when not viewing SCREEN_PET).
// If you find yourself wanting to add an `lv_*` call here — put it in
// pet_screen.cpp instead, or you'll be doing widget work on hidden objects
// from every loop iteration.
void pet_tick(void) {
    // Idle XP decay was removed in the API-only-XP rebalance — care-stat
    // decay (below) is now the sole "ignore the device and bad things
    // happen" mechanic. XP only ever moves up; care stats only move down
    // unless the player intervenes.
    uint32_t now = pet_now();

    // ----- Vacation auto-expiry (check BEFORE the decay block) -----
    // If the 14-day window has elapsed, cancel vacation so the same tick
    // runs the normal decay branch immediately. Prefer wall-clock so a long
    // power-off counts toward the limit (P1-4); fall back to monotonic when
    // no daemon ever pushed a clock.
    if (g_vacation_active) {
        uint32_t elapsed;
        uint32_t wc_now = pet_wallclock_now();
        if (wc_now > 0 && g_vacation_started_wallclock > 0) {
            elapsed = (wc_now > g_vacation_started_wallclock)
                        ? wc_now - g_vacation_started_wallclock : 0;
        } else {
            elapsed = now - g_vacation_started_pet_now;
        }
        if (elapsed >= (uint32_t)VACATION_MAX_DAYS * 86400u) {
            pet_set_vacation(false);
            Serial.println("[pet] vacation auto-expired (14d limit)");
        }
    }

    // ----- Care stat decay -----
    // Each stat decays at STAT_DECAY_INTERVAL seconds independently.
    // Loop handles long power-on idle periods (catches up rather than
    // dropping decay ticks). Decay clocks were rebased on init/migrate so
    // a fresh boot doesn't immediately punish a returning user.
    auto decay_one = [](uint8_t* stat, uint32_t* ts, uint32_t now, uint32_t interval) {
        bool dirty = false;
        while (now > *ts && (now - *ts) > interval) {
            if (*stat > 0) { (*stat)--; dirty = true; }
            *ts += interval;
        }
        return dirty;
    };
    // Snapshot pre-decay values so we can detect transitions to 0.
    uint8_t pre_s = g_current.satiety;
    uint8_t pre_p = g_current.spirit;
    uint8_t pre_b = g_current.bond;

    if (!g_vacation_active) {
        // Normal decay — stats count down each STAT_DECAY_INTERVAL.
        uint32_t interval = effective_decay_interval(now);
        if (decay_one(&g_current.satiety, &g_current.satiety_decay_ts, now, interval)) g_dirty = true;
        if (decay_one(&g_current.spirit,  &g_current.spirit_decay_ts,  now, interval)) g_dirty = true;
        if (decay_one(&g_current.bond,    &g_current.bond_decay_ts,    now, interval)) g_dirty = true;
    } else {
        // Vacation: advance decay anchors to now so that when vacation ends
        // stats don't immediately drop for the accumulated offline time.
        g_current.satiety_decay_ts = now;
        g_current.spirit_decay_ts  = now;
        g_current.bond_decay_ts    = now;
    }

    // Neglect event fires on the transition where any stat just hit 0.
    bool any_just_hit_zero = (pre_s > 0 && g_current.satiety == 0) ||
                             (pre_p > 0 && g_current.spirit  == 0) ||
                             (pre_b > 0 && g_current.bond    == 0);
    if (any_just_hit_zero && g_baselines.neglect_event_armed) {
        Serial.println("pet: NEGLECTED — fire event");
        g_baselines.neglect_event_armed = 0;
        fire_event(PET_EVENT_NEGLECTED);
    }

    // Re-arm when all stats are comfortably above the rearm threshold.
    if (!g_baselines.neglect_event_armed &&
        g_current.satiety > NEGLECT_REARM_THRESHOLD &&
        g_current.spirit  > NEGLECT_REARM_THRESHOLD &&
        g_current.bond    > NEGLECT_REARM_THRESHOLD) {
        g_baselines.neglect_event_armed = 1;
    }

    // ----- NVS flush cadence -----
    // Flush if we're past the routine interval since last flush, OR if any
    // event/XP gain has marked the state dirty since last flush.
    //
    // P2-4: defer the routine (non-dirty) flush while splash animations are
    // active. NVS writes momentarily stall the main loop; splash is the most
    // animation-heavy screen and a stall there is the most visible. Dirty
    // flushes (level-up, XP gain, care events) are NOT deferred — losing
    // those to a power-cycle is worse than a brief micro-jitter.
    static uint32_t last_flush_ts = 0;
    bool routine_due = (now - last_flush_ts) >= PET_NVS_FLUSH_INTERVAL;
    if (routine_due && !g_dirty && splash_is_active()) {
        // skip this tick; routine will retry on the next pet_tick call
        routine_due = false;
    }
    if (g_dirty || routine_due) {
        save_nvs();   // also rebases pet_now() internally
        last_flush_ts = pet_now();
        g_dirty = false;
    }
}

static void fire_event(pet_event_t ev) {
    g_dirty = true;
    if (g_event_cb_set && g_event_cb) {
        g_event_cb(ev);
    } else {
        // Callback not registered yet — queue for replay when callback is registered.
        g_pending_events |= (1u << (uint32_t)ev);
    }
}

// Saturating stat adjustment. Both return the actual delta applied
// (always non-negative for add, non-positive for sub) so callers that
// need to detect "did this hit a cap" can check the return.
// Both also refresh the corresponding decay clock on any positive
// adjustment so manual/passive refill "freshens" the decay countdown.
static uint8_t stat_add(uint8_t* stat, uint8_t delta, uint32_t* decay_ts) {
    if (!stat || delta == 0) return 0;
    uint16_t sum = (uint16_t)(*stat) + delta;
    uint8_t actual;
    if (sum >= 100) { actual = (uint8_t)(100 - *stat); *stat = 100; }
    else            { actual = delta;                  *stat = (uint8_t)sum; }
    if (actual > 0 && decay_ts) *decay_ts = pet_now();
    return actual;
}

// Find a per-session baseline by sid. Returns -1 if not present.
static int find_baseline_idx(const char* sid) {
    for (uint8_t i = 0; i < g_baselines.ses_baseline_count; i++) {
        if (strncmp(g_baselines.ses[i].id, sid, 9) == 0) return (int)i;
    }
    return -1;
}

// Rebuild the per-session baseline map from the current SessionList so
// sessions that dropped off the active window don't keep stale baselines.
// New sessions enter with baseline 0; their first reported c% becomes the
// initial XP delta.
static void rebuild_baselines_from(const SessionList* sl) {
    PetBaselines next = {};
    next.last_s_pct = g_baselines.last_s_pct;
    next.last_w_pct = g_baselines.last_w_pct;
    next.ses_baseline_count = 0;
    for (uint8_t i = 0; i < sl->count && i < SESSION_MAX; i++) {
        strlcpy(next.ses[next.ses_baseline_count].id, sl->items[i].id,
                sizeof(next.ses[next.ses_baseline_count].id));
        // Record the CURRENT observed ctx_pct as the new baseline. The
        // pet_on_metrics caller has already computed any positive delta
        // against the old baseline; subsequent pushes will only award XP
        // for further upward movement.
        next.ses[next.ses_baseline_count].c_pct = sl->items[i].ctx_pct;
        next.ses_baseline_count++;
    }
    // Phase 3 — carry forward care baselines so threshold edge-detection
    // and neglect-arming survive each metrics push.
    next.last_satiety_passive_ts    = g_baselines.last_satiety_passive_ts;
    next.prev_5h_crossed_80         = g_baselines.prev_5h_crossed_80;
    next.prev_weekly_crossed_80     = g_baselines.prev_weekly_crossed_80;
    next.prev_session_count         = g_baselines.prev_session_count;
    next.prev_compacting_count_seen = g_baselines.prev_compacting_count_seen;
    next.neglect_event_armed        = g_baselines.neglect_event_armed;
    g_baselines = next;
}

// Snapshot the current pet to the gallery (evicting oldest if at cap) and
// spawn a fresh L1 pet via round-robin species rotation.
static void graduate_current(void) {
    // Total XP earned by this pet across all levels reached.
    uint32_t total_xp = g_current.xp;
    for (uint8_t L = 1; L < g_current.level; L++) {
        total_xp += pet_xp_to_next(L);
    }

    Graduate g = {};
    g.species_id      = g_current.species_id;
    g.born_ts         = g_current.born_ts;
    g.graduated_ts    = pet_now();
    g.total_xp_earned = total_xp;
    // V4 fields — carry name and shiny from the graduating pet.
    memcpy(g.name, g_current.name, sizeof(g.name));
    g.name[sizeof(g.name) - 1] = '\0';
    g.is_shiny        = g_current.is_shiny;
    // V5: capture a signature quote so the F12 bio modal has something
    // personal to show. Uses the pet's current mood-category line at
    // graduate-time + a stable salt so this graduate always gets the
    // same farewell line. Empty if pet_speech_pick returns null.
    {
        const char* line = pet_speech_pick(pet_speech_current_category(),
                                           (uint32_t)g.graduated_ts);
        if (line) {
            strncpy(g.quote, line, sizeof(g.quote) - 1);
            g.quote[sizeof(g.quote) - 1] = '\0';
        }
    }

    if (g_gallery.count >= PET_GALLERY_CAP) {
        // Evict oldest — shift entries[1..N-1] down to [0..N-2].
        memmove(&g_gallery.entries[0], &g_gallery.entries[1],
                sizeof(Graduate) * (PET_GALLERY_CAP - 1));
        g_gallery.entries[PET_GALLERY_CAP - 1] = g;
    } else {
        g_gallery.entries[g_gallery.count++] = g;
    }

    Serial.printf("pet: GRADUATED species=%u total_xp=%lu (gallery=%u)\n",
        g.species_id, (unsigned long)g.total_xp_earned, g_gallery.count);
    pet_mark_species_discovered(g.species_id, g.is_shiny != 0);

    // Round-robin next species across all 13 claudepix creatures (phase 2).
    uint16_t next_species = (g_current.species_id + 1) % PET_NUM_SPECIES;
    instantiate_pet(next_species);
    Serial.printf("pet: new pet spawned species=%u\n", g_current.species_id);

    // One consolidated flush covering: graduating pet (in gallery), new pet,
    // both discovery-mask updates, and any shiny recap — replaces the ~200ms
    // cascade of stacked individual Preferences open/close calls that used to
    // happen synchronously during graduation.
    save_nvs();

    fire_event(PET_EVENT_GRADUATE);
}

void pet_on_metrics(const UsageData* u, const SessionList* s) {
    if (!u || !u->valid || !s) return;

    // Care stats (Saturation / Spirit / Bond) are deliberately NOT touched
    // by API metrics — the player is solely responsible for tending them
    // via the feed/play/pet actions. API usage only feeds XP and level.

    // First-payload seeding: when the pet is fresh (L1 / 0 XP / no session
    // baselines yet), the daemon's first push carries CUMULATIVE metrics
    // accumulated before the pet was alive. Treating those as deltas-from-0
    // grants the user a huge retroactive XP windfall (observed: instant L7
    // from one payload after an NVS erase). Absorb the values to seed the
    // baselines, but skip the XP award.
    bool fresh_pet = (g_current.level == 1 && g_current.xp == 0
                   && g_baselines.ses_baseline_count == 0
                   && g_baselines.last_s_pct == 0
                   && g_baselines.last_w_pct == 0);

    uint32_t xp_gained = 0;

    // 5h utilization delta (clamp negatives — window reset is not XP loss).
    // Defensive: clamp the float to [0,100] before the uint8_t cast so a
    // malformed payload (e.g. NaN, >254.5) can't trigger undefined cast behaviour.
    float s_raw = u->session_pct;
    if (!(s_raw >= 0.0f)) s_raw = 0.0f;          // NaN-safe via inverted compare
    if (s_raw > 100.0f)  s_raw = 100.0f;
    uint8_t s_pct = (uint8_t)(s_raw + 0.5f);
    if (s_pct > g_baselines.last_s_pct) {
        xp_gained += (uint32_t)(s_pct - g_baselines.last_s_pct) * XP_PER_5H_PCT;
    }
    g_baselines.last_s_pct = s_pct;

    // Weekly utilization delta.
    float w_raw = u->weekly_pct;
    if (!(w_raw >= 0.0f)) w_raw = 0.0f;
    if (w_raw > 100.0f)  w_raw = 100.0f;
    uint8_t w_pct = (uint8_t)(w_raw + 0.5f);
    if (w_pct > g_baselines.last_w_pct) {
        xp_gained += (uint32_t)(w_pct - g_baselines.last_w_pct) * XP_PER_WEEKLY_PCT;
    }
    g_baselines.last_w_pct = w_pct;

    // Per-session context deltas — find existing baseline (or 0 if new),
    // compute positive delta, then rebuild the baseline map to the current
    // SessionList so dropped sessions don't leak baseline slots.
    for (uint8_t i = 0; i < s->count; i++) {
        const SessionInfo& si = s->items[i];
        int prev_idx = find_baseline_idx(si.id);
        uint8_t baseline = (prev_idx >= 0) ? g_baselines.ses[prev_idx].c_pct : 0;
        if (si.ctx_pct > baseline) {
            xp_gained += (uint32_t)(si.ctx_pct - baseline) * XP_PER_SESSION_CTX_PCT;
        }
    }
    rebuild_baselines_from(s);

    // First payload after a fresh pet: baselines are now populated, but no
    // XP is awarded. Persist so we don't re-seed on next boot.
    if (fresh_pet) {
        if (xp_gained > 0) {
            Serial.printf("pet: first-payload seed — discarding %lu XP (would have leveled fresh pet)\n",
                (unsigned long)xp_gained);
        }
        g_dirty = true;
        return;
    }

    if (xp_gained == 0) return;

    // ---------- Care-stat XP multiplier ----------
    // Three-tier system: average rewards balance, low-stat ladder punishes
    // neglect non-linearly, perfect-care unlocks a 3x ceiling.
    //   100/100/100 → 300%   (perfect — peak care)
    //    90/90/90   → 225%
    //    80/80/80   → 176%
    //    50/50/50   → 100%   (baseline)
    //    80/80/30   → 100%   (one weak stat undoes a "good" pet)
    //    80/80/10   →  21%
    //    any 0      →   0%   (no XP until you tend them)
    uint8_t s_sat = g_current.satiety;
    uint8_t s_spi = g_current.spirit;
    uint8_t s_bon = g_current.bond;
    uint8_t min_stat = s_sat;
    if (s_spi < min_stat) min_stat = s_spi;
    if (s_bon < min_stat) min_stat = s_bon;
    uint16_t avg = ((uint16_t)s_sat + s_spi + s_bon) / 3;
    uint32_t mul_pct = (uint32_t)avg * 2;   // 0..200 base from average

    if      (min_stat == 0)  mul_pct = 0;
    else if (min_stat < 10)  mul_pct = (mul_pct * 25) / 100;
    else if (min_stat < 25)  mul_pct = (mul_pct * 50) / 100;
    else if (min_stat < 50)  mul_pct = (mul_pct * 80) / 100;

    if      (s_sat == 100 && s_spi == 100 && s_bon == 100) mul_pct = (mul_pct * 150) / 100;
    else if (avg >= 90)  mul_pct = (mul_pct * 125) / 100;
    else if (avg >= 75)  mul_pct = (mul_pct * 110) / 100;

    uint32_t xp_raw = xp_gained;
    xp_gained = (uint32_t)(((uint64_t)xp_gained * mul_pct) / 100);
    Serial.printf("pet: xp mul=%lu%% (sat=%u spi=%u bon=%u avg=%u min=%u): %lu → %lu\n",
        (unsigned long)mul_pct, s_sat, s_spi, s_bon, (unsigned)avg, min_stat,
        (unsigned long)xp_raw, (unsigned long)xp_gained);

    if (xp_gained == 0) return;

    g_current.xp           += xp_gained;
    g_current.last_event_ts = pet_now();
    g_dirty                 = true;

    recap_note_xp_gain(xp_gained);

    Serial.printf("pet: +%lu XP (now L%u xp=%lu)\n",
        (unsigned long)xp_gained, g_current.level, (unsigned long)g_current.xp);

    // Level-up: each level resets XP to 0 — no carry-over. A single huge
    // grant therefore only awards one level. Cascading multi-level jumps
    // from one payload are intentionally disallowed.
    while (g_current.level < PET_MAX_LEVEL) {
        uint32_t cost = pet_xp_to_next(g_current.level);
        if (g_current.xp < cost) break;
        g_current.xp = 0;
        g_current.level++;
        g_current.last_level_up_pet_now = pet_now();   // for stats screen history line
        Serial.printf("pet: LEVEL UP → L%u (xp reset to 0)\n", g_current.level);
        fire_event(PET_EVENT_LEVEL_UP);
        // Notify achievements synchronously: ach_lv30 must be evaluated here
        // because graduate_current() resets level=1 before achievements_tick
        // can see level 30.
        achievements_notify_levelup(g_current.level);
        // Achievement week counter (task 2.3)
        week_rollover_check();
        g_week_levelups++;

        // Evolution milestone?
        if (g_current.level == 4 || g_current.level == 9 ||
            g_current.level == 17 || g_current.level == 30) {
            Serial.printf("pet: EVOLVE → stage=%u\n", pet_evolution_stage(&g_current));
            fire_event(PET_EVENT_EVOLVE);
        }

        // Graduation? Loop will exit via the level < MAX condition on the
        // next iteration; the xp=0 reset above ensures no cascade past here.
        if (g_current.level == PET_MAX_LEVEL) {
            graduate_current();
            break;
        }
    }
}

const Pet*     pet_current(void)  { return &g_current; }
const Gallery* pet_gallery(void)  { return &g_gallery; }
// Level curve: gently exponential. xp_to_next(L) = 200 * L * 1.05^L.
// L1→2 = 210, L29→30 ≈ 23 873. Total to graduate ≈ 200 k XP. Curve was
// 1.08 originally — that back-loaded late levels so hard (4–8× the early
// per-level cost) that Stage 4 evolution took weeks of grind. 1.05 keeps
// progression visible without trivializing the final stretch.
uint32_t pet_xp_to_next(uint8_t level) {
    if (level == 0 || level >= PET_MAX_LEVEL) return UINT32_MAX;  // never advances past max
    double base = 200.0 * (double)level;
    double exp_factor = pow(1.05, (double)level);
    return (uint32_t)(base * exp_factor);
}

// Map level to sprite tier. Evolutions at L1/L4/L9/L17/L30 — 5 stages,
// rebalanced from L5/L12/L22/L30 so the visible reward arrives at a more
// regular cadence (3 / 5 / 8 / 13 level gaps instead of 4 / 7 / 10 / 8).
uint8_t pet_evolution_stage(const Pet* p) {
    if (!p)              return 0;
    if (p->level >= 30)  return 4;
    if (p->level >= 17)  return 3;
    if (p->level >= 9)   return 2;
    if (p->level >= 4)   return 1;
    return 0;
}

void pet_set_event_cb(void (*cb)(pet_event_t)) {
    g_event_cb = cb;
    g_event_cb_set = cb ? 1 : 0;
    // Replay any events that fired before the callback was registered (e.g.
    // PET_EVENT_HATCHED on first boot, which fires inside pet_init() before
    // ui_init() + pet_set_event_cb() run).
    if (cb && g_pending_events != 0) {
        for (uint32_t i = 0; i <= (uint32_t)PET_EVENT_HATCHED; i++) {
            if (g_pending_events & (1u << i)) {
                cb((pet_event_t)i);
            }
        }
        g_pending_events = 0;
    }
}

uint8_t pet_species_animation_index(uint16_t species_id) {
    return (uint8_t)(species_id % PET_NUM_SPECIES);
}

const char* pet_species_name(uint16_t species_id) {
    static const char* NAMES[PET_NUM_SPECIES] = {
        "Bytey",       // 0
        "Loopster",    // 1
        "Forktail",    // 2
        "Mergosaurus", // 3
        "Patchwork",   // 4
        "Cachet",      // 5
        "Hashling",    // 6
        "Suderpus",    // 7
        "Cronfox",     // 8
        "Pickleer",    // 9
        "Sprintsaur",  // 10
        "Branchowl",   // 11
        "Commitster",  // 12
    };
    return NAMES[species_id % PET_NUM_SPECIES];
}

uint32_t pet_total_xp_earned(void) {
    uint32_t total = g_current.xp;
    for (uint8_t L = 1; L < g_current.level; L++) {
        total += pet_xp_to_next(L);
    }
    return total;
}

// ============== Manual actions (phase 3) ==============

// Helper for compute-cooldown-remaining. Returns 0 when ready.
static uint32_t cooldown_remaining(uint32_t last_ts) {
    if (last_ts == 0) return 0;
    uint32_t now = pet_now();
    if (now < last_ts) return 0;                          // clock weirdness — treat as ready
    uint32_t elapsed = now - last_ts;
    if (elapsed >= ACTION_COOLDOWN_SEC) return 0;
    return ACTION_COOLDOWN_SEC - elapsed;
}

bool pet_feed(void) {
    if (cooldown_remaining(g_last_feed_ts) > 0) return false;
    if (g_current.satiety >= 100) return false;
    stat_add(&g_current.satiety, ACTION_REFILL_AMOUNT, &g_current.satiety_decay_ts);
    g_last_feed_ts = pet_now();
    g_dirty = true;   // lifetime counter + stat change flushed by pet_tick cadence
    // Achievement counters (task 2.3)
    g_lifetime_feed++;
    week_rollover_check();
    g_week_feed++;
    // Streak (task 3.1)
    streak_note_care_action();
    // Recap (task 3.2)
    recap_note_care_action();
    Serial.printf("pet: FEED (+%u → satiety=%u) lifetime=%u week=%u\n",
        ACTION_REFILL_AMOUNT, g_current.satiety,
        (unsigned)g_lifetime_feed, (unsigned)g_week_feed);
    return true;
}

bool pet_play(void) {
    if (cooldown_remaining(g_last_play_ts) > 0) return false;
    if (g_current.spirit >= 100) return false;
    stat_add(&g_current.spirit, ACTION_REFILL_AMOUNT, &g_current.spirit_decay_ts);
    g_last_play_ts = pet_now();
    g_dirty = true;   // lifetime counter + stat change flushed by pet_tick cadence
    // Achievement counters (task 2.3)
    g_lifetime_play++;
    week_rollover_check();
    g_week_play++;
    // Streak (task 3.1)
    streak_note_care_action();
    // Recap (task 3.2)
    recap_note_care_action();
    Serial.printf("pet: PLAY (+%u → spirit=%u) lifetime=%u week=%u\n",
        ACTION_REFILL_AMOUNT, g_current.spirit,
        (unsigned)g_lifetime_play, (unsigned)g_week_play);
    return true;
}

bool pet_pet(void) {
    if (cooldown_remaining(g_last_pet_ts) > 0) return false;
    if (g_current.bond >= 100) return false;
    stat_add(&g_current.bond, ACTION_REFILL_AMOUNT, &g_current.bond_decay_ts);
    g_last_pet_ts = pet_now();
    g_dirty = true;   // lifetime counter + stat change flushed by pet_tick cadence
    // Achievement counters (task 2.3)
    g_lifetime_pet++;
    week_rollover_check();
    // Streak (task 3.1)
    streak_note_care_action();
    // Recap (task 3.2)
    recap_note_care_action();
    Serial.printf("pet: PET (+%u → bond=%u) lifetime=%u\n",
        ACTION_REFILL_AMOUNT, g_current.bond, (unsigned)g_lifetime_pet);
    return true;
}

PetStats pet_stats(void) {
    PetStats s;
    s.satiety                  = g_current.satiety;
    s.spirit                   = g_current.spirit;
    s.bond                     = g_current.bond;
    s.feed_cooldown_remaining  = cooldown_remaining(g_last_feed_ts);
    s.play_cooldown_remaining  = cooldown_remaining(g_last_play_ts);
    s.pet_cooldown_remaining   = cooldown_remaining(g_last_pet_ts);
    return s;
}

void pet_rename(const char* name) {
    if (!name || !name[0]) return;   // empty string — keep existing name
    strncpy(g_current.name, name, sizeof(g_current.name) - 1);
    g_current.name[sizeof(g_current.name) - 1] = '\0';
    g_dirty = true;
    save_nvs();   // immediate flush so the name survives a power cycle
    Serial.printf("pet: RENAMED → \"%s\"\n", g_current.name);
}

size_t pet_get_name(char* dst, size_t cap) {
    if (!dst || cap == 0) return 0;
    size_t src_len = strnlen(g_current.name, sizeof(g_current.name));
    size_t copy = (src_len < cap - 1) ? src_len : cap - 1;
    memcpy(dst, g_current.name, copy);
    dst[copy] = '\0';
    return copy;
}

void pet_set_shiny(bool on) {
    g_current.is_shiny = on ? 1 : 0;
    g_dirty = true;
    // No immediate save_nvs here — the routine tick flush covers it, and
    // a missed power-cycle between the cmd and the next flush is acceptable.
    // If the user wants a guaranteed persist, they can use "restart" after.
    Serial.printf("pet: is_shiny → %u\n", (unsigned)g_current.is_shiny);
}

void pet_test_clear_and_rehatch(void) {
    // Wipe the persisted current-pet record so a future cold boot would also
    // spawn fresh, then instantiate a new L1 pet immediately on the next
    // round-robin species. PET_EVENT_HATCHED fires inside instantiate_pet,
    // which the pet_screen event hook turns into the naming-keyboard pop.
    {
        Preferences prefs;
        if (prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
            prefs.remove(NVS_KEY_CURRENT);
            prefs.end();
        }
    }
    uint16_t next_species = (uint16_t)((g_current.species_id + 1) % PET_NUM_SPECIES);
    instantiate_pet(next_species);
    g_dirty = true;
    save_nvs();
    Serial.printf("pet: SIM-WIPE → fresh L1 species=%u name=%s\n",
                  next_species, g_current.name);
}

void pet_force_graduate(void) {
    graduate_current();
}

void pet_test_populate_graduates(uint8_t n) {
    for (uint8_t i = 0; i < n; i++) {
        if (g_gallery.count >= PET_GALLERY_CAP) break;
        Graduate& gr = g_gallery.entries[g_gallery.count];
        gr = {};
        gr.species_id = (uint16_t)(g_gallery.count % PET_NUM_SPECIES);
        gr.born_ts = pet_now() - 86400u * 30u;
        gr.graduated_ts = pet_now() - 86400u * (uint32_t)(10u - i);
        gr.total_xp_earned = 5000u + ((uint32_t)i * 800u);
        gr.is_shiny = ((esp_random() & 0x03u) == 0u) ? 1 : 0;
        snprintf(gr.name, sizeof(gr.name), "Test%u", (unsigned)g_gallery.count);
        g_gallery.count++;
    }
    g_dirty = true;
    save_nvs();
    Serial.printf("pet: test graduates populated, count=%u\n", (unsigned)g_gallery.count);
}
