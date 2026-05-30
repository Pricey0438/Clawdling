#include "catalog_screen.h"
#include "pet.h"
#include "splash.h"
#include "theme.h"
#include "ui.h"
#include "hal/board_caps.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

// Grid layout constants.
#define CATALOG_COLS      4
#define CATALOG_ROWS      4
#define CATALOG_NUM_CELLS (CATALOG_COLS * CATALOG_ROWS)   // 16 total, 13 used

// Cell size in splash grid units (20×20 grid). cell=3 → 60×60 px sprite.
// On the large (480×480) board: (480 - 2*12 - 3*8) / 4 = ~104 px per cell
// slot, so a 60 px sprite + name label fits comfortably.
#define CATALOG_SPRITE_CELL  3
#define CATALOG_SPRITE_PX    (20 * CATALOG_SPRITE_CELL)   // 60

static lv_obj_t*  s_root       = nullptr;   // full-screen container
static lv_obj_t*  s_header_lbl = nullptr;   // "X of 13 discovered • Y shinies"

// Per-cell sprite canvas objects and their PSRAM pixel buffers.
// Allocated once in catalog_screen_init(); refreshed (pixel data only)
// on every catalog_screen_show() call. Only cells 0..12 are used.
static lv_obj_t*  s_cell_canvas[PET_NUM_SPECIES] = {};
static uint16_t*  s_cell_bufs[PET_NUM_SPECIES]   = {};

// Shiny indicators (small ★ label overlaid on a discovered shiny cell).
static lv_obj_t*  s_shiny_lbl[PET_NUM_SPECIES]   = {};

// ---- helpers ----

// Fill a PSRAM pixel buffer with a solid colour (RGB565).
static void fill_buf_color(uint16_t* buf, int count, uint16_t color565) {
    for (int i = 0; i < count; i++) buf[i] = color565;
}

// Refresh the header label to current discovery counts.
static void refresh_header(void) {
    if (!s_header_lbl) return;
    char buf[48];
    // ASCII separator — bitmap fonts lack U+2022 •
    snprintf(buf, sizeof(buf), "%u of %u discovered  |  %u shinies",
             (unsigned)pet_discovered_count(),
             (unsigned)PET_NUM_SPECIES,
             (unsigned)pet_discovered_shiny_count());
    lv_label_set_text(s_header_lbl, buf);
}

// Refresh all cell pixel data from the current discovery bitmasks.
// Canvas widget data is updated in-place (no widget destroy/recreate).
static void refresh_cells(void) {
    uint16_t disc_mask  = pet_discovered_species_mask();
    uint16_t shiny_mask = pet_discovered_shiny_mask();

    for (int sp = 0; sp < PET_NUM_SPECIES; sp++) {
        if (!s_cell_bufs[sp]) continue;

        bool discovered = (disc_mask  & (1u << sp)) != 0;
        bool shiny      = (shiny_mask & (1u << sp)) != 0;

        if (discovered) {
            uint8_t anim_idx = pet_species_animation_index((uint16_t)sp);
            splash_render_to_buf(s_cell_bufs[sp], CATALOG_SPRITE_PX,
                                 CATALOG_SPRITE_CELL, anim_idx, 0, shiny);
        } else {
            // Undiscovered — solid dark grey silhouette.
            fill_buf_color(s_cell_bufs[sp],
                           CATALOG_SPRITE_PX * CATALOG_SPRITE_PX,
                           0x2104 /* ~#202020 in RGB565 */);
        }

        // Invalidate so LVGL re-draws on next render pass.
        if (s_cell_canvas[sp]) lv_obj_invalidate(s_cell_canvas[sp]);

        // Show/hide shiny indicator.
        if (s_shiny_lbl[sp]) {
            if (shiny) lv_obj_clear_flag(s_shiny_lbl[sp], LV_OBJ_FLAG_HIDDEN);
            else       lv_obj_add_flag  (s_shiny_lbl[sp], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ---- public API ----

void catalog_screen_init(lv_obj_t* parent) {
    if (s_root) return;   // already built

    const BoardCaps& caps = board_caps();
    bool large = (caps.height >= 460);

    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, caps.width, caps.height);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, THEME_BG, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    int sbh = theme_status_bar_h();

    // --- Header ---
    s_header_lbl = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_header_lbl, theme_font_label(), 0);
    lv_obj_set_style_text_color(s_header_lbl, THEME_DIM, 0);
    lv_label_set_text(s_header_lbl, "");
    lv_obj_align(s_header_lbl, LV_ALIGN_TOP_MID, 0, sbh + SP_S);

    // --- Grid ---
    // Compute cell slot size: (screen_w - 2*margin) / 4 cols.
    int margin    = SP_XL;
    int grid_w    = caps.width - 2 * margin;
    int slot_w    = grid_w / CATALOG_COLS;

    // Reserve a few rows at the top for header + status bar.
    int grid_top  = sbh + (large ? 48 : 40);
    int grid_h    = caps.height - grid_top - SP_M;
    int slot_h    = grid_h / CATALOG_ROWS;

    // Name label height — reserve ~20px at the bottom of each slot.
    int name_h    = large ? 22 : 18;
    // Vertical gap between sprite canvas top and the slot top (centering).
    int sprite_y_off = (slot_h - CATALOG_SPRITE_PX - name_h - SP_XS) / 2;
    if (sprite_y_off < 0) sprite_y_off = 0;

    for (int sp = 0; sp < PET_NUM_SPECIES; sp++) {
        int col = sp % CATALOG_COLS;
        int row = sp / CATALOG_COLS;

        int cell_x = margin + col * slot_w;
        int cell_y = grid_top + row * slot_h;

        // Allocate PSRAM pixel buffer for this cell's sprite canvas.
        s_cell_bufs[sp] = (uint16_t*)heap_caps_malloc(
            CATALOG_SPRITE_PX * CATALOG_SPRITE_PX * 2, MALLOC_CAP_SPIRAM);
        if (!s_cell_bufs[sp]) {
            Serial.printf("catalog: PSRAM alloc failed for species %d\n", sp);
            continue;
        }
        // Zero-fill so undiscovered cells show black before first refresh.
        memset(s_cell_bufs[sp], 0, CATALOG_SPRITE_PX * CATALOG_SPRITE_PX * 2);

        // Canvas widget (sprite area).
        int canvas_x = cell_x + (slot_w - CATALOG_SPRITE_PX) / 2;
        int canvas_y = cell_y + sprite_y_off;

        s_cell_canvas[sp] = lv_canvas_create(s_root);
        lv_canvas_set_buffer(s_cell_canvas[sp], s_cell_bufs[sp],
                             CATALOG_SPRITE_PX, CATALOG_SPRITE_PX,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(s_cell_canvas[sp], canvas_x, canvas_y);
        lv_obj_clear_flag(s_cell_canvas[sp], LV_OBJ_FLAG_CLICKABLE);

        // Shiny indicator — small amber ★ in the top-right corner.
        s_shiny_lbl[sp] = lv_label_create(s_root);
        lv_label_set_text(s_shiny_lbl[sp], "\xE2\x98\x85");   // ★ U+2605
        lv_obj_set_style_text_font(s_shiny_lbl[sp], theme_font_label(), 0);
        lv_obj_set_style_text_color(s_shiny_lbl[sp], THEME_AMBER, 0);
        lv_obj_set_pos(s_shiny_lbl[sp],
                       canvas_x + CATALOG_SPRITE_PX - 12,
                       canvas_y);
        lv_obj_add_flag(s_shiny_lbl[sp], LV_OBJ_FLAG_HIDDEN);

        // Species name label (below sprite).
        int name_y = canvas_y + CATALOG_SPRITE_PX + SP_XS;
        lv_obj_t* name_lbl = lv_label_create(s_root);
        lv_obj_set_style_text_font(name_lbl, theme_font_label(), 0);
        lv_obj_set_style_text_color(name_lbl, THEME_DIM, 0);
        lv_obj_set_width(name_lbl, slot_w - 4);
        lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(name_lbl, LV_TEXT_ALIGN_CENTER, 0);

        // Name text — initially set to "???" for all; refresh_cells sets real names.
        // We keep a single static text for discovered and re-set at show-time via
        // a simpler approach: name label text is always the real name (or "???"),
        // but we set it here once and leave it — species names never change.
        bool discovered_now = (pet_discovered_species_mask() & (1u << sp)) != 0;
        lv_label_set_text(name_lbl,
            discovered_now ? pet_species_name((uint16_t)sp) : "???");
        lv_obj_set_pos(name_lbl,
                       cell_x + 2,
                       name_y);

        // Store pointer so we can update text at show-time.
        // Piggyback on s_shiny_lbl's parent — we use lv_obj_get_child by index
        // via the lv_obj child API. Simpler: keep a separate array.
        // Actually, re-using tag: store name_lbl as user data on the canvas.
        lv_obj_set_user_data(s_cell_canvas[sp], (void*)name_lbl);
    }

    Serial.printf("catalog: init done (%d species cells)\n", PET_NUM_SPECIES);
}

void catalog_screen_show(void) {
    if (!s_root) return;

    // Refresh header counts.
    refresh_header();

    // Refresh sprites + shiny indicators. Also sync name labels (discovered
    // species show real name; undiscovered show "???").
    uint16_t disc_mask = pet_discovered_species_mask();
    for (int sp = 0; sp < PET_NUM_SPECIES; sp++) {
        if (!s_cell_canvas[sp]) continue;
        lv_obj_t* name_lbl = (lv_obj_t*)lv_obj_get_user_data(s_cell_canvas[sp]);
        if (name_lbl) {
            bool discovered = (disc_mask & (1u << sp)) != 0;
            lv_label_set_text(name_lbl,
                discovered ? pet_species_name((uint16_t)sp) : "???");
        }
    }
    refresh_cells();

    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    ui_status_bar_set_title("CATALOG");
}

void catalog_screen_hide(void) {
    if (!s_root) return;
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}
