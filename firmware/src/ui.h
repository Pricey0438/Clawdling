#pragma once
#include "data.h"
#include "ble.h"

enum screen_t {
    SCREEN_SPLASH,
    SCREEN_USAGE,
    SCREEN_SESSION,
    SCREEN_PET,
    SCREEN_MENU,         // phase 4 — was SCREEN_BLUETOOTH (still shows BLE
                          // status until T5; T5 adds the menu-list view)
    SCREEN_CATALOG,       // phase 2.2 — species catalog grid
    SCREEN_ACHIEVEMENTS,  // phase 2.3 — achievements + weekly quests
    SCREEN_PROVISIONING,  // WiFi setup: QR + AP SSID + form URL
    SCREEN_RETIREMENT,    // phase 4.1 — retirement home, multi-pet graduate tank
    SCREEN_STATS,         // foundation plan — read-only depth panel reached from SCREEN_PET
    SCREEN_OOBE,          // wave 6a — first-boot QR shown when no daemon + no wifi
    SCREEN_COUNT,
};

// Phase 4 — abstract button identity (decoupled from input_hal's INPUT_BTN_*
// and power_hal's PWR press). Drives the screen-aware dispatcher below.
typedef enum {
    UI_BTN_LEFT,
    UI_BTN_MID,
    UI_BTN_RIGHT,
} ui_btn_t;

// Press-edge dispatcher. Main loop calls this once per press transition.
// `pressed` is true on press, false on release (release is currently
// ignored — kept in the signature so per-screen handlers can opt in later).
void ui_input_event(ui_btn_t btn, bool pressed);

void ui_init(void);
void ui_update(const UsageData* data);
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
void ui_cycle_screen(void);
void ui_toggle_splash(void);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);
// Refresh the WiFi glyph in the status bar (hidden / white=fresh / amber=stale).
// Call whenever WiFi config or daemon freshness may have changed.
void ui_update_wifi(void);
// Status bar title — called by per-screen show/drill paths in T3 to set
// the left-zone label (e.g. "USAGE", "SESSIONS", "‹ SESSIONS" for sub-views).
// Pass NULL to clear (used implicitly when SCREEN_SPLASH hides the bar).
void ui_status_bar_set_title(const char* text);

// Device-wide lock. While locked, the device forces SCREEN_SPLASH and
// shows a "[ LOCKED ]" pill. Button + touch dispatch is gated by the
// caller (main.cpp checks ui_is_locked() before forwarding events).
// Triggered by the LEFT+RIGHT combo (2.16 only; 1.8 has no RIGHT button).
void     ui_set_locked(bool locked);
bool     ui_is_locked(void);
void     ui_toggle_lock(void);

// Back affordance for the status-bar title. When `cb` is non-null, the
// title label becomes clickable and tapping it fires `cb` (typically the
// per-screen back-out path). Pass nullptr to clear; per-screen modules
// should pair set_title / set_back_cb in their drill and back-out paths.
void ui_status_bar_set_back_cb(void (*cb)(void));

// Provisioning screen — shows QR code for the SoftAP SSID + form URL hint.
// Creates and loads a fresh LVGL screen; canvas buffer allocated from PSRAM
// and intentionally leaked (device reboots when provisioning completes).
void ui_show_provisioning(const String& ap_ssid);
