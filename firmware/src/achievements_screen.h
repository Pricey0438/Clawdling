#pragma once
#include <lvgl.h>

// Achievements screen — scrollable list of all achievements with unlock state.
// Header shows "X of 18 unlocked". Active weekly quest is starred.
void achievements_screen_init(lv_obj_t* parent);   // build once, hidden
void achievements_screen_show(void);                // make visible, refresh data
void achievements_screen_hide(void);                // hide
