#pragma once
#include <lvgl.h>

// Species catalog screen — 4×4 grid (13 cells used, 3 trailing empty).
// Each discovered species shows its sprite at frame 0; undiscovered shows
// a dark rectangle with "???" label. Header shows discovery progress.
void catalog_screen_init(lv_obj_t* parent);   // build the screen once, hidden
void catalog_screen_show(void);                // make visible, refresh data
void catalog_screen_hide(void);                // hide
