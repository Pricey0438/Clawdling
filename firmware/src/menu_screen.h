#pragma once
#include "ui.h"     // for ui_btn_t
#include <lvgl.h>

// Initialise the menu screen widgets inside `root` (the menu_container).
// `bluetooth_subview` is a sibling sub-container managed by ui.cpp whose
// visibility menu_screen toggles when drilling into the Bluetooth item.
// Called once from ui.cpp's init_menu_screen.
void menu_screen_init(lv_obj_t* root, lv_obj_t* bluetooth_subview);

// Called by ui_show_screen(SCREEN_MENU) when the screen becomes active.
// Resets to the list view and refreshes dynamic item secondary text
// (Gallery count, BLE state).
void menu_screen_show(void);

// Re-evaluate dynamic secondary labels (e.g., when BLE state changes
// or graduates accumulate). Cheap to call repeatedly.
void menu_screen_refresh_secondary_labels(void);

// Per-tick refresh (currently no-op; reserved for future).
void menu_screen_tick(void);

// Called by ui.cpp's ui_show_screen when leaving SCREEN_MENU. Frees any
// PSRAM-allocated gallery thumbnails and resets the view to the list.
void menu_screen_reset_view(void);

// Button dispatcher hook. Returns true if consumed.
bool menu_screen_on_button(ui_btn_t btn);
