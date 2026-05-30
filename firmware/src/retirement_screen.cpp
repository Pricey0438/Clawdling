#include "retirement_screen.h"
#include "pet.h"
#include "splash.h"
#include "theme.h"
#include "ui.h"
#include "hal/board_caps.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

// Grid: 4 columns × 2 rows = 8 slots.
#define RET_COLS       4
#define RET_ROWS       2
#define RET_MAX_SLOTS  (RET_COLS * RET_ROWS)   // 8

// Sprite grid cell size in splash grid units (20×20 grid per anim).
// 3 → 60×60 px sprite, same as catalog. Computed per-board in init.
static int s_cell_px  = 3;   // grid units (cell_size argument to splash_render_to_buf)
static int s_sprite_px = 0;  // 20 * s_cell_px, pixels per canvas axis

// 4 fps — calm, ambient vibe.
#define RET_FRAME_MS   250

// ---- Widget tree (built once, hidden by default) ----
static lv_obj_t*  s_root       = nullptr;
static lv_obj_t*  s_header_lbl = nullptr;   // "Retirement Home" title
static lv_obj_t*  s_sub_lbl    = nullptr;   // "X of Y" or empty
static lv_obj_t*  s_empty_lbl  = nullptr;   // shown when gallery is empty
static lv_obj_t*  s_grid_obj   = nullptr;   // container for cells

// Per-slot: canvas widget + pixel buffer + name label + shiny badge.
static lv_obj_t*  s_canvas[RET_MAX_SLOTS]  = {};
static uint16_t*  s_bufs[RET_MAX_SLOTS]    = {};
static lv_obj_t*  s_name_lbl[RET_MAX_SLOTS]= {};
static lv_obj_t*  s_shiny_lbl[RET_MAX_SLOTS]= {};

// Which graduates are in which slot. -1 = unused slot.
static int        s_grad_idx[RET_MAX_SLOTS] = {};

// ---- Animation state ----
static uint16_t  s_frame_idx    = 0;
static uint32_t  s_last_frame_ms = 0;
static bool      s_built        = false;    // true once init has run

// ---- Bio modal (F12 — P0-1 / P1-15) ----
// Full-screen overlay shown when the user taps any populated retirement
// tile. Renders the graduate's bio entirely from local data: large sprite
// (re-using splash_render_to_buf), name, species, born/graduated info,
// total XP, and the signature quote captured at graduate-time. No HTTP
// round-trip; the gallery is firmware-owned, so the daemon's
// /graduate.svg endpoint exists only as a host-side helper for sharing.
static lv_obj_t*  s_bio_modal     = nullptr;
static uint16_t*  s_bio_sprite_buf = nullptr;
static int        s_bio_sprite_px  = 0;

static void bio_modal_hide(void) {
    if (!s_bio_modal) return;
    lv_obj_delete(s_bio_modal);
    s_bio_modal = nullptr;
    // Sprite buffer is allocated per-show; free on dismiss to avoid leaking
    // a PSRAM chunk per tile-tap.
    if (s_bio_sprite_buf) {
        heap_caps_free(s_bio_sprite_buf);
        s_bio_sprite_buf = nullptr;
    }
}

static void bio_dismiss_cb(lv_event_t* e) {
    (void)e;
    bio_modal_hide();
}

static void show_bio_modal(int grad_idx);   // forward decl

static void tile_tap_cb(lv_event_t* e) {
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    if (slot < 0 || slot >= RET_MAX_SLOTS) return;
    int gi = s_grad_idx[slot];
    if (gi < 0) return;
    show_bio_modal(gi);
}

// ---- helpers ----

static void free_bufs(void) {
    for (int i = 0; i < RET_MAX_SLOTS; i++) {
        if (s_bufs[i]) {
            heap_caps_free(s_bufs[i]);
            s_bufs[i] = nullptr;
        }
        s_canvas[i] = nullptr;
    }
}

static void redraw_slot(int slot) {
    if (!s_bufs[slot] || !s_canvas[slot]) return;
    int gi = s_grad_idx[slot];
    if (gi < 0) return;
    const Gallery* g = pet_gallery();
    if (!g || gi >= (int)g->count) return;
    const Graduate& gr = g->entries[gi];
    uint8_t anim_idx = pet_species_animation_index(gr.species_id);
    splash_render_to_buf(s_bufs[slot], s_sprite_px, s_cell_px,
                         anim_idx, s_frame_idx, gr.is_shiny != 0);
    lv_obj_invalidate(s_canvas[slot]);
}

// Fill all slots with data from the gallery (called on show).
static void populate_slots(void) {
    const Gallery* g = pet_gallery();
    uint16_t total = g ? g->count : 0;

    // Choose which 8 graduates to display: most-recent up to 8.
    uint16_t display_start = 0;   // gallery index of the first displayed grad
    uint16_t display_count = total;
    if (display_count > RET_MAX_SLOTS) {
        display_count = RET_MAX_SLOTS;
        display_start = total - RET_MAX_SLOTS;  // highest indices = most recent
    }

    // ---- Header / subtitle ----
    char sub_buf[32];
    if (total == 0) {
        lv_label_set_text(s_sub_lbl, "");
    } else if (total > RET_MAX_SLOTS) {
        snprintf(sub_buf, sizeof(sub_buf), "%u of %u", (unsigned)RET_MAX_SLOTS, (unsigned)total);
        lv_label_set_text(s_sub_lbl, sub_buf);
    } else {
        snprintf(sub_buf, sizeof(sub_buf), "%u graduate%s",
                 (unsigned)total, total == 1 ? "" : "s");
        lv_label_set_text(s_sub_lbl, sub_buf);
    }

    // ---- Empty-state label ----
    if (total == 0) {
        if (s_empty_lbl)  lv_obj_clear_flag(s_empty_lbl, LV_OBJ_FLAG_HIDDEN);
        if (s_grid_obj)   lv_obj_add_flag(s_grid_obj,   LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (s_empty_lbl)  lv_obj_add_flag(s_empty_lbl, LV_OBJ_FLAG_HIDDEN);
    if (s_grid_obj)   lv_obj_clear_flag(s_grid_obj,   LV_OBJ_FLAG_HIDDEN);

    // Allocate / refresh slots.
    for (int slot = 0; slot < RET_MAX_SLOTS; slot++) {
        bool active = slot < (int)display_count;
        int  gi     = active ? (int)(display_start + slot) : -1;
        s_grad_idx[slot] = gi;

        // Hide slot widgets if out of range.
        if (s_canvas[slot]) {
            if (active) lv_obj_clear_flag(s_canvas[slot], LV_OBJ_FLAG_HIDDEN);
            else        lv_obj_add_flag  (s_canvas[slot], LV_OBJ_FLAG_HIDDEN);
        }
        if (s_name_lbl[slot]) {
            if (active) lv_obj_clear_flag(s_name_lbl[slot], LV_OBJ_FLAG_HIDDEN);
            else        lv_obj_add_flag  (s_name_lbl[slot], LV_OBJ_FLAG_HIDDEN);
        }
        if (s_shiny_lbl[slot]) {
            if (active) lv_obj_clear_flag(s_shiny_lbl[slot], LV_OBJ_FLAG_HIDDEN);
            else        lv_obj_add_flag  (s_shiny_lbl[slot], LV_OBJ_FLAG_HIDDEN);
        }

        if (!active) continue;

        // Allocate PSRAM buffer if not yet done.
        if (!s_bufs[slot] && s_canvas[slot]) {
            s_bufs[slot] = (uint16_t*)heap_caps_malloc(
                s_sprite_px * s_sprite_px * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        }

        if (s_bufs[slot] && s_canvas[slot]) {
            lv_canvas_set_buffer(s_canvas[slot], s_bufs[slot],
                                 s_sprite_px, s_sprite_px, LV_COLOR_FORMAT_RGB565);
            redraw_slot(slot);
        }

        // Update name label.
        if (s_name_lbl[slot] && g && gi < (int)g->count) {
            const Graduate& gr = g->entries[gi];
            const char* nm = gr.name[0] ? gr.name : pet_species_name(gr.species_id);
            lv_label_set_text(s_name_lbl[slot], nm);
        }

        // Update shiny badge.
        if (s_shiny_lbl[slot] && g && gi < (int)g->count) {
            const Graduate& gr = g->entries[gi];
            if (gr.is_shiny)  lv_obj_clear_flag(s_shiny_lbl[slot], LV_OBJ_FLAG_HIDDEN);
            else              lv_obj_add_flag  (s_shiny_lbl[slot], LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Reset frame so sprites start in sync on first render.
    s_frame_idx     = 0;
    s_last_frame_ms = lv_tick_get();
}

// ---- Public API ----

void retirement_screen_init(lv_obj_t* parent) {
    if (s_built) return;
    s_built = true;

    const BoardCaps& caps = board_caps();
    bool large = (caps.height >= 460);

    // ---- Cell geometry ----
    // Reserve header (~50 px) + name row below each sprite.
    // Divide remaining height by 2 rows; divide width by 4 cols.
    // Square the cell to whichever dimension is constraining.
    int header_h  = large ? 52 : 44;
    int name_h    = large ? 28 : 24;
    int grid_pad  = large ? 12 : 8;
    int grid_gap  = large ? 10 : 6;
    int avail_h   = caps.height - header_h - 2 * grid_pad;
    int cell_slot_h = (avail_h - grid_gap) / 2;           // height per slot including name
    int sprite_slot = cell_slot_h - name_h - grid_gap;    // pixels available for sprite
    int avail_w   = caps.width  - 2 * grid_pad;
    int col_w     = (avail_w - 3 * grid_gap) / 4;        // pixel width of each column

    // Pick the smallest dimension so the sprite is square.
    int max_sprite = (sprite_slot < col_w) ? sprite_slot : col_w;
    // Round down to nearest multiple of 20 (grid size) with minimum 1.
    s_cell_px  = max_sprite / 20;
    if (s_cell_px < 1) s_cell_px = 1;
    s_sprite_px = 20 * s_cell_px;

    // ---- Root container ----
    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, caps.width, caps.height);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, THEME_BG, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Header ----
    int sbh = theme_status_bar_h();

    // Body header ("Retirement Home") removed — redundant with the status-bar
    // title set in retirement_screen_show(). s_header_lbl kept null so the
    // refresh_header() and other accessors continue to no-op safely.
    s_header_lbl = nullptr;

    // Subtitle: centered under the status bar (was right-aligned next to the
    // dropped header — caused visual crowding on 480x480, see Wave 5 audit).
    s_sub_lbl = lv_label_create(s_root);
    lv_label_set_text(s_sub_lbl, "");
    lv_obj_set_style_text_font(s_sub_lbl, theme_font_label(), 0);
    lv_obj_set_style_text_color(s_sub_lbl, THEME_DIM, 0);
    lv_obj_align(s_sub_lbl, LV_ALIGN_TOP_MID, 0, sbh + SP_XS);

    // ---- Empty-state label ----
    s_empty_lbl = lv_label_create(s_root);
    lv_label_set_text(s_empty_lbl,
        "No graduates yet —\nyour first pet retires at Lv 30.");
    lv_obj_set_style_text_font(s_empty_lbl, theme_font_body(), 0);
    lv_obj_set_style_text_color(s_empty_lbl, THEME_DIM, 0);
    lv_obj_set_style_text_align(s_empty_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_empty_lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_empty_lbl, LV_OBJ_FLAG_HIDDEN);  // shown only when count==0

    // ---- Grid container ----
    s_grid_obj = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_grid_obj);
    lv_obj_set_size(s_grid_obj, avail_w, avail_h);
    lv_obj_set_pos(s_grid_obj, grid_pad, header_h + grid_pad);
    lv_obj_clear_flag(s_grid_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_grid_obj, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_grid_obj, LV_OBJ_FLAG_HIDDEN);   // revealed by populate_slots

    // ---- Per-slot widgets ----
    // Center the sprite within each column slot; name centered below.
    for (int row = 0; row < RET_ROWS; row++) {
        for (int col = 0; col < RET_COLS; col++) {
            int slot = row * RET_COLS + col;

            // Column x position (left edge of slot).
            int x = col * (col_w + grid_gap);
            // Row y position (top of sprite).
            int y = row * (cell_slot_h + grid_gap);

            // Center sprite in the column slot.
            int sx = x + (col_w  - s_sprite_px) / 2;
            int sy = y;

            // Canvas for the animated sprite. CLICKABLE so a tap opens the
            // graduate's bio modal (F12 — P1-15). user_data carries the slot
            // index so the shared tile_tap_cb can look up the graduate.
            s_canvas[slot] = lv_canvas_create(s_grid_obj);
            lv_obj_set_size(s_canvas[slot], s_sprite_px, s_sprite_px);
            lv_obj_set_pos(s_canvas[slot], sx, sy);
            lv_obj_add_flag(s_canvas[slot], LV_OBJ_FLAG_CLICKABLE);
            // ext_click_area grows the hit-target so a finger tap doesn't
            // have to land on the exact 60×60 sprite pixels — important on
            // AMOLED-1.8 where sprites shrink to 80×80 with smaller gaps.
            lv_obj_set_ext_click_area(s_canvas[slot], 8);
            lv_obj_add_event_cb(s_canvas[slot], tile_tap_cb,
                                LV_EVENT_CLICKED, (void*)(intptr_t)slot);

            // Name label (centered under the sprite).
            s_name_lbl[slot] = lv_label_create(s_grid_obj);
            lv_label_set_text(s_name_lbl[slot], "");
            lv_obj_set_style_text_font(s_name_lbl[slot], theme_font_label(), 0);
            lv_obj_set_style_text_color(s_name_lbl[slot], THEME_TEXT, 0);
            lv_obj_set_width(s_name_lbl[slot], col_w);
            lv_label_set_long_mode(s_name_lbl[slot], LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(s_name_lbl[slot], LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(s_name_lbl[slot], x, y + s_sprite_px + 2);

            // Shiny badge — tiny "★" overlaid top-right of sprite.
            s_shiny_lbl[slot] = lv_label_create(s_grid_obj);
            lv_label_set_text(s_shiny_lbl[slot], "\xE2\x98\x85");   // ★ U+2605
            lv_obj_set_style_text_font(s_shiny_lbl[slot], theme_font_label(), 0);
            lv_obj_set_style_text_color(s_shiny_lbl[slot], THEME_AMBER, 0);
            // Position: top-right corner of the sprite.
            lv_obj_set_pos(s_shiny_lbl[slot], sx + s_sprite_px - 14, sy);
            lv_obj_add_flag(s_shiny_lbl[slot], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// Build and show the bio modal for a specific graduate index. Modal is
// parented to lv_scr_act() so it overlays the retirement screen; dismissed
// via tap-anywhere via bio_dismiss_cb.
static void show_bio_modal(int grad_idx) {
    const Gallery* g = pet_gallery();
    if (!g || grad_idx < 0 || grad_idx >= (int)g->count) return;
    const Graduate& gr = g->entries[grad_idx];

    bio_modal_hide();   // tear down any prior modal

    const BoardCaps& caps = board_caps();

    // ----- Full-screen dim overlay -----
    s_bio_modal = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_bio_modal);
    lv_obj_set_size(s_bio_modal, caps.width, caps.height);
    lv_obj_set_pos(s_bio_modal, 0, 0);
    lv_obj_set_style_bg_color(s_bio_modal, THEME_BG, 0);
    lv_obj_set_style_bg_opa(s_bio_modal, LV_OPA_90, 0);
    lv_obj_add_flag(s_bio_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_bio_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_bio_modal);
    lv_obj_add_event_cb(s_bio_modal, bio_dismiss_cb, LV_EVENT_CLICKED, nullptr);

    // ----- Centered card -----
    const int card_margin = SP_M;
    const int card_w      = caps.width  - 2 * card_margin;
    const int card_h      = caps.height - 2 * card_margin - theme_status_bar_h();
    lv_obj_t* card = lv_obj_create(s_bio_modal);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, card_w, card_h);
    lv_obj_set_pos(card, card_margin, theme_status_bar_h() + card_margin);
    lv_obj_set_style_bg_color(card, THEME_PANEL, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_border_color(card, THEME_ACCENT, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    // Swallow taps on the card so they don't trip the overlay dismiss.
    lv_obj_add_event_cb(card, [](lv_event_t* e){ (void)e; }, LV_EVENT_CLICKED, nullptr);

    int y = SP_M;
    const int pad_x = SP_M;
    const int lbl_w = card_w - 2 * pad_x;

    // ----- Sprite (large) -----
    int big_cell = caps.height >= 460 ? 6 : 5;   // 120x120 / 100x100
    s_bio_sprite_px = 20 * big_cell;
    size_t buf_bytes = (size_t)s_bio_sprite_px * s_bio_sprite_px * sizeof(uint16_t);
    s_bio_sprite_buf = (uint16_t*)heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    if (s_bio_sprite_buf) {
        uint8_t anim_idx = pet_species_animation_index(gr.species_id);
        splash_render_to_buf(s_bio_sprite_buf, s_bio_sprite_px, big_cell,
                             anim_idx, 0, gr.is_shiny != 0);
        lv_obj_t* canv = lv_canvas_create(card);
        lv_obj_set_size(canv, s_bio_sprite_px, s_bio_sprite_px);
        lv_canvas_set_buffer(canv, s_bio_sprite_buf, s_bio_sprite_px,
                             s_bio_sprite_px, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(canv, (card_w - s_bio_sprite_px) / 2, y);
        lv_obj_clear_flag(canv, LV_OBJ_FLAG_CLICKABLE);
        y += s_bio_sprite_px + SP_S;
    }

    // ----- Name + shiny badge -----
    {
        char namebuf[24];
        snprintf(namebuf, sizeof(namebuf), "%s%s",
                 gr.name[0] ? gr.name : pet_species_name(gr.species_id),
                 gr.is_shiny ? " \xE2\x98\x85" : "");
        lv_obj_t* lbl = lv_label_create(card);
        lv_obj_set_style_text_font(lbl, theme_font_display_s(), 0);
        lv_obj_set_style_text_color(lbl, THEME_ACCENT, 0);
        lv_obj_set_width(lbl, lbl_w);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(lbl, namebuf);
        lv_obj_set_pos(lbl, pad_x, y);
        y += 30 + SP_XS;
    }

    // ----- Species line -----
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%s", pet_species_name(gr.species_id));
        lv_obj_t* lbl = lv_label_create(card);
        lv_obj_set_style_text_font(lbl, theme_font_body(), 0);
        lv_obj_set_style_text_color(lbl, THEME_DIM, 0);
        lv_obj_set_width(lbl, lbl_w);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(lbl, buf);
        lv_obj_set_pos(lbl, pad_x, y);
        y += 22;
    }

    // ----- Lifespan (in days) + total XP -----
    {
        uint32_t span_secs = (gr.graduated_ts > gr.born_ts)
                              ? (gr.graduated_ts - gr.born_ts) : 0;
        uint32_t span_days = span_secs / 86400u;
        char buf[48];
        snprintf(buf, sizeof(buf), "%ud lived  %lu XP",
                 (unsigned)span_days, (unsigned long)gr.total_xp_earned);
        lv_obj_t* lbl = lv_label_create(card);
        lv_obj_set_style_text_font(lbl, theme_font_label(), 0);
        lv_obj_set_style_text_color(lbl, THEME_TEXT, 0);
        lv_obj_set_width(lbl, lbl_w);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(lbl, buf);
        lv_obj_set_pos(lbl, pad_x, y);
        y += 20 + SP_S;
    }

    // ----- Quote (V5 field, may be empty) -----
    if (gr.quote[0]) {
        char qbuf[80];
        snprintf(qbuf, sizeof(qbuf), "\"%s\"", gr.quote);
        lv_obj_t* lbl = lv_label_create(card);
        lv_obj_set_style_text_font(lbl, theme_font_label(), 0);
        lv_obj_set_style_text_color(lbl, THEME_AMBER, 0);
        lv_obj_set_width(lbl, lbl_w);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(lbl, qbuf);
        lv_obj_set_pos(lbl, pad_x, y);
    }

    // ----- Tap-to-close hint -----
    {
        lv_obj_t* lbl = lv_label_create(card);
        lv_obj_set_style_text_font(lbl, theme_font_label(), 0);
        lv_obj_set_style_text_color(lbl, THEME_DIM, 0);
        lv_obj_set_width(lbl, lbl_w);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(lbl, "tap to close");
        lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -SP_S);
    }

    Serial.printf("[retirement] bio modal: %s (sp=%u, xp=%lu)\n",
                  gr.name[0] ? gr.name : pet_species_name(gr.species_id),
                  (unsigned)gr.species_id, (unsigned long)gr.total_xp_earned);
}

void retirement_screen_show(void) {
    if (!s_root) return;
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    ui_status_bar_set_title("RETIREMENT");
    populate_slots();
}

void retirement_screen_hide(void) {
    if (!s_root) return;
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}

void retirement_screen_hide_modals(void) {
    bio_modal_hide();
}

void retirement_screen_tick(void) {
    if (!s_root) return;
    if (lv_obj_has_flag(s_root, LV_OBJ_FLAG_HIDDEN)) return;

    uint32_t now = lv_tick_get();
    if (now - s_last_frame_ms < RET_FRAME_MS) return;
    s_last_frame_ms = now;
    s_frame_idx++;

    // Redraw all active slots with the new frame index.
    for (int slot = 0; slot < RET_MAX_SLOTS; slot++) {
        if (s_grad_idx[slot] >= 0) {
            redraw_slot(slot);
        }
    }
}
