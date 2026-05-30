#pragma once
#include <lvgl.h>
#include "hal/board_caps.h"

LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_styrene_48);  // status_xl large
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);

// ====== Color tokens ======
// Anthropic-inspired dark palette, AMOLED-friendly (true black bg).
#define THEME_BG       lv_color_hex(0x000000)   // screen background
#define THEME_PANEL    lv_color_hex(0x1f1f1e)   // card/zone fill
#define THEME_TEXT     lv_color_hex(0xfaf9f5)   // primary text
#define THEME_DIM      lv_color_hex(0xb0aea5)   // secondary text
#define THEME_ACCENT   lv_color_hex(0xd97757)   // brand terra-cotta
#define THEME_GREEN    lv_color_hex(0x788c5d)
#define THEME_AMBER    lv_color_hex(0xd97757)
#define THEME_PURPLE   lv_color_hex(0xb57edc)   // compacting state
#define THEME_RED      lv_color_hex(0xc0392b)
#define THEME_BAR_BG   lv_color_hex(0x2a2a28)   // unfilled bar track

// ====== Spacing scale (4 px grid) ======
#define SP_XS   4
#define SP_S    8
#define SP_M   12
#define SP_L   16
#define SP_XL  20   // matches the existing L.margin

// ====== Status bar dimensions ======
// 40 px on the 480-tall AMOLED-2.16, 32 px on the 448-tall AMOLED-1.8.
// Selected by the same H >= 460 breakpoint compute_layout() uses.
static inline int theme_status_bar_h(void) {
    return board_caps().height >= 460 ? 40 : 32;
}

// ====== Type roles ======
// Per-board font for each semantic role. Helpers compile at call-time and
// return a const lv_font_t*. Use these instead of LV_FONT_DECLARE'ing the
// raw font in each screen file.
static inline const lv_font_t* theme_font_display(void) {
    return board_caps().height >= 460 ? &font_tiempos_56 : &font_tiempos_34;
}
static inline const lv_font_t* theme_font_display_s(void) {
    return board_caps().height >= 460 ? &font_styrene_28 : &font_styrene_20;
}
static inline const lv_font_t* theme_font_body(void) {
    return board_caps().height >= 460 ? &font_styrene_24 : &font_styrene_20;
}
static inline const lv_font_t* theme_font_label(void) {
    return board_caps().height >= 460 ? &font_styrene_16 : &font_styrene_14;
}
static inline const lv_font_t* theme_font_status_xl(void) {
    return board_caps().height >= 460 ? &font_styrene_48 : &font_styrene_28;
}
static inline const lv_font_t* theme_font_credit_1(void) {
    return board_caps().height >= 460 ? &font_styrene_24 : &font_styrene_16;
}
static inline const lv_font_t* theme_font_credit_2(void) {
    return board_caps().height >= 460 ? &font_styrene_20 : &font_styrene_14;
}
