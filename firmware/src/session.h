#pragma once
#include "data.h"
#include "ui.h"   // for ui_btn_t
#include <lvgl.h>

// Build the SCREEN_SESSION widget tree inside the given parent container.
// Follows the existing container-with-hidden-flag pattern used by the other
// screens in ui.cpp (usage_container, ble_container, ...) — does NOT create
// a separate top-level lv_screen.
void session_screen_init(lv_obj_t* parent);

// Called whenever a new SessionList arrives. Repopulates the list view and
// (in detail view, once implemented) refreshes the focused session's fields.
void session_screen_update(const SessionList* list);

// Animation tick — called from ui_tick_anim(). No-op for the list view;
// advances the Clawd pixel-art animation in the detail view.
void session_screen_tick_anim(void);

// Reset the screen back to the list view (called when PWR-cycling away so
// the next entry to SCREEN_SESSION starts from the list, not detail).
void session_screen_reset_view(void);

// Phase 4 — button dispatcher hook. Returns true if consumed (focused-
// session advance, drill into detail, or back-nav from detail). Returns
// false to let ui.cpp cycle screens (Mid default on list view).
bool session_on_button(ui_btn_t btn);
