#pragma once

// Shared visual primitives used by USAGE and SESSION cards.
// Implementations live in ui.cpp — this header just publishes them.

#include <lvgl.h>

// A rounded panel sized (w, h) at (x, y) relative to `parent`, painted in
// THEME_PANEL with the standard SP_L horizontal / SP_M vertical padding.
lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h);

// A 0..100 progress bar with the THEME_BAR_BG track and THEME_GREEN
// indicator by default. Callers re-tint the indicator after creation if
// they need a state-aware color.
lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h);

// USAGE-style two-row card. Populates out_pct (big numeric), out_pill
// (top-right rounded label), out_bar (24px progress bar), out_reset
// (subtitle). Caller owns layout y — height is L.usage_panel_h internally.
// NOTE: depends on ui.cpp's LayoutVars `L` being initialized by ui_init().
// Must not be called before ui_init() completes.
void make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                      lv_obj_t** out_pct, lv_obj_t** out_pill,
                      lv_obj_t** out_bar, lv_obj_t** out_reset);

// A stat card: rounded panel with a 24×24 icon top-left, big numeric value
// left (offset to clear the icon), short pill top-right, full-width bar
// chained above a bottom-aligned subtitle. Card height is configurable so
// callers can pack 3-up on PET Care Detail (~110-130px) without rewriting
// USAGE. Bar position adapts to font metrics via lv_obj_align_to so the
// helper survives both the large (H>=460) and compact breakpoints.
//
// NOTE: depends on ui.cpp's LayoutVars `L` being initialized by ui_init().
// Must not be called before ui_init() completes.
void make_stat_panel(lv_obj_t* parent, int y, int h,
                     const lv_image_dsc_t* icon,
                     const char* pill_text,
                     lv_obj_t** out_value,
                     lv_obj_t** out_pill,
                     lv_obj_t** out_bar,
                     lv_obj_t** out_subtitle);
