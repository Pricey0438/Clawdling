#pragma once
#include <lvgl.h>
#include "ui.h"

// Build the screen container under `parent`. Idempotent — call from
// the existing ensure_*_screen() pattern in ui.cpp. Stores the container
// internally; subsequent calls are no-ops.
void pet_stats_screen_init(lv_obj_t* parent);

// Show the screen — refreshes labels from pet/streak/achievements/recap
// accessors. Build is done by init; this only reads + re-fills.
void pet_stats_screen_show(void);

// Hide the screen. Called by the ui.cpp screen-switch path when leaving.
void pet_stats_screen_hide(void);

// Button event hook. Return true if the button was consumed.
// Currently: left/back → return to SCREEN_PET; right/mid → unhandled.
bool pet_stats_screen_on_button(ui_btn_t btn);

// Test/sim entrypoint: run the same tap-to-refill code path that a real
// touch on the Sat (idx=0) or Spi (idx=1) card would trigger, without
// needing a physical tap. Used by the `sim tap-stat <i>` serial cmd.
void pet_stats_screen_sim_tap(int idx);
