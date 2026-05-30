#pragma once
#include "lvgl.h"

// Retirement Home screen — shows up to 8 graduated pets as animated sprites
// in a 4×2 grid with a shared frame timer (calm 4 fps).
void retirement_screen_init(lv_obj_t* parent);   // build the screen, hidden
void retirement_screen_show(void);                // make visible, populate from gallery
void retirement_screen_hide(void);                // hide
void retirement_screen_tick(void);                // advance shared animation frame; call from main loop

// Tear down the bio-card modal if one is open (parented to lv_scr_act so
// it'd otherwise leak across screen cycles). Safe to call when no modal.
void retirement_screen_hide_modals(void);
