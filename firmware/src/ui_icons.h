#pragma once

// Shared lv_image_dsc_t initialisers for the two icon formats used by
// the firmware. Per-screen modules hold their own descriptor statics
// (so each TU keeps its icons alive for that screen's lifetime); these
// helpers just populate the header/data fields consistently.
//
// RGB565   — legacy uint16_t[] arrays (bluetooth, trash). No alpha.
// RGB565A8 — planar uint8_t[] arrays: w*h RGB565 pixels then w*h alpha
//            bytes; total = w*h*3. Used for the Lucide glyphs that
//            overlay non-uniform backgrounds.

#include <lvgl.h>
#include <stdint.h>

static inline void ui_icon_init_rgb565(lv_image_dsc_t* dsc, int w, int h,
                                       const uint16_t* data) {
    dsc->header.w      = w;
    dsc->header.h      = h;
    dsc->header.cf     = LV_COLOR_FORMAT_RGB565;
    dsc->header.stride = w * 2;
    dsc->data          = (const uint8_t*)data;
    dsc->data_size     = w * h * 2;
}

static inline void ui_icon_init_rgb565a8(lv_image_dsc_t* dsc, int w, int h,
                                         const uint8_t* data) {
    dsc->header.w      = w;
    dsc->header.h      = h;
    dsc->header.cf     = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data          = data;
    dsc->data_size     = w * h * 3;
}
