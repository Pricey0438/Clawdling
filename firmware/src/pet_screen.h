#pragma once
#include "pet.h"
#include "ui.h"
#include <lvgl.h>

// Build the SCREEN_PET widget tree inside the given parent container.
// Follows the existing container-with-hidden-flag pattern used by the other
// screens in ui.cpp.
void pet_screen_init(lv_obj_t* parent);

// Per-frame tick — animates the pet sprite + overlays, decays toast timer.
// Called from ui_tick_anim() when SCREEN_PET is the active screen.
void pet_screen_tick(void);

// Called by ui_show_screen on entry to SCREEN_PET. Refreshes labels and
// re-renders the sprite so each entry starts in a consistent state.
void pet_screen_show(void);

// Phase 4 — button dispatcher hook. Returns true if the screen consumed
// the press (e.g., advanced the Sat↔Spi cursor, committed Feed/Play).
// Returns false to let the ui.cpp dispatcher do default Mid=cycle behavior.
bool pet_screen_on_button(ui_btn_t btn);

// Internal event callback registered with pet_set_event_cb at boot.
// Flashes a transient toast (parented to the PET screen root, so visible
// from both PET_HOME and PET_CARE_DETAIL) if SCREEN_PET is active.
void pet_screen_on_event(pet_event_t ev);

// Force an immediate speech bubble with the given text. Used by the `speech`
// serial cmd and by future quest/achievement triggers. Safe to call from any
// context while SCREEN_PET is active; no-ops silently if the widget isn't built.
void pet_screen_force_speech_bubble(const char* text);

// Force-show the daily recap modal regardless of recap_should_show() state.
// Used by the `recap` serial cmd for screenshot testing.
void pet_screen_force_recap(void);

// Tear down any modal currently parented to lv_scr_act (recap modal today;
// any future modal owned by SCREEN_PET should be added here). Called by
// ui_show_screen when leaving SCREEN_PET so modals can't leak across cycles.
// Safe to call when no modal is up.
void pet_screen_hide_modals(void);

// Public wrapper around the internal toast widget — flashes the given text
// for ~2s on the pet screen's toast label. No-op if the screen isn't built.
// Used by the achievements-unlock callback to surface unlocks visually (P1-5).
// Text is copied into the LVGL label; caller's buffer can go out of scope.
void pet_screen_flash_toast(const char* text);
