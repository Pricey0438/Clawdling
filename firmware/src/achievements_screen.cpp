#include "achievements_screen.h"
#include "achievements.h"
#include "theme.h"
#include "ui.h"
#include "hal/board_caps.h"
#include "paged_nav.h"
#include "icons.h"
#include "ui_icons.h"   // ui_icon_init_rgb565a8 lives here (NOT ui.h)
#include <Arduino.h>

// Icon-tile grid: 2 columns × 2 rows = 4 tiles/page, rebuilt per page so only
// ~4 tiles (×3 objects) are live at once — heap-safe on the ~18-24 KB budget.
// Unlocked tiles show a trophy + name; locked tiles a lock + "??????"; the
// active weekly quest is tinted amber. Tapping a tile opens a detail overlay.

static lv_obj_t*  s_root       = nullptr;
static lv_obj_t*  s_header_lbl = nullptr;
static lv_obj_t*  s_grid_obj   = nullptr;   // holds the current page's tiles
static lv_obj_t*  s_detail     = nullptr;   // tap-detail overlay
static PagedNav   s_nav;

// Grid geometry, computed once in init and reused by every page rebuild.
static int s_grid_w = 0;
static int s_grid_h = 0;

// Mirror the main menu grid (the reference design standard): 2 cols × 3 rows.
#define ACH_COLS       2
#define ACH_ROWS       3
#define ACH_PER_PAGE   (ACH_COLS * ACH_ROWS)   // 6 tiles/page

static lv_image_dsc_t s_trophy_dsc;
static lv_image_dsc_t s_lock_dsc;
static bool           s_icons_inited = false;

static void ach_init_icons(void) {
    if (s_icons_inited) return;
    s_icons_inited = true;
    ui_icon_init_rgb565a8(&s_trophy_dsc, ICON_TROPHY_W, ICON_TROPHY_H, icon_trophy_data);
    ui_icon_init_rgb565a8(&s_lock_dsc,   ICON_LOCK_W,   ICON_LOCK_H,   icon_lock_data);
}

// ---- header ----

static void refresh_header(void) {
    if (!s_header_lbl) return;
    char buf[48];
    snprintf(buf, sizeof(buf), "%u of %u unlocked",
             (unsigned)achievements_unlocked_count(),
             (unsigned)ACHIEVEMENTS_COUNT);
    lv_label_set_text(s_header_lbl, buf);
}

// ---- tap-detail overlay ----

static void ach_detail_hide(void) {
    if (!s_detail) return;
    lv_obj_delete(s_detail);
    s_detail = nullptr;
}
static void ach_detail_dismiss_cb(lv_event_t* e) { (void)e; ach_detail_hide(); }

static void ach_show_detail(int ach_index) {
    if (ach_index < 0 || ach_index >= ACHIEVEMENTS_COUNT) return;
    const Achievement* a = &ACHIEVEMENTS[ach_index];
    bool unlocked = achievement_is_unlocked(a->id);
    ach_detail_hide();

    const BoardCaps& caps = board_caps();
    s_detail = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_detail);
    lv_obj_set_size(s_detail, caps.width, caps.height);
    lv_obj_set_pos(s_detail, 0, 0);
    lv_obj_set_style_bg_color(s_detail, THEME_BG, 0);
    lv_obj_set_style_bg_opa(s_detail, LV_OPA_90, 0);
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_detail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_detail);
    lv_obj_add_event_cb(s_detail, ach_detail_dismiss_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* icon = lv_image_create(s_detail);
    lv_image_set_src(icon, unlocked ? &s_trophy_dsc : &s_lock_dsc);
    lv_obj_set_style_image_recolor(icon, unlocked ? THEME_ACCENT : THEME_DIM, 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, theme_status_bar_h() + SP_XL);

    lv_obj_t* name = lv_label_create(s_detail);
    lv_obj_set_style_text_font(name, theme_font_display_s(), 0);
    lv_obj_set_style_text_color(name, unlocked ? THEME_ACCENT : THEME_DIM, 0);
    lv_label_set_text(name, unlocked ? a->name : "Locked");
    lv_obj_align(name, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t* desc = lv_label_create(s_detail);
    lv_obj_set_style_text_font(desc, theme_font_body(), 0);
    lv_obj_set_style_text_color(desc, THEME_TEXT, 0);
    lv_obj_set_width(desc, caps.width - 2 * SP_XL);
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(desc, a->description);
    lv_obj_align(desc, LV_ALIGN_CENTER, 0, 40);

    lv_obj_t* hint = lv_label_create(s_detail);
    lv_obj_set_style_text_font(hint, theme_font_label(), 0);
    lv_obj_set_style_text_color(hint, THEME_DIM, 0);
    lv_label_set_text(hint, "tap to close");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -SP_L);
}

static void ach_tile_tap_cb(lv_event_t* e) {
    int ach_index = (int)(intptr_t)lv_event_get_user_data(e);
    ach_show_detail(ach_index);
}

// ---- per-page tile rebuild ----

// Rebuild ONLY the current page's tiles. Heap-safe: <=6 tiles live at once.
// Tile structure mirrors the main menu grid (build_grid_view): a centered
// flex column of icon -> primary label -> status, THEME_PANEL, radius 12.
static void ach_render_page(int page) {
    if (!s_grid_obj) return;
    lv_obj_clean(s_grid_obj);

    uint8_t active_quest = weekly_quest_active_id();
    int gap    = SP_M;
    int tile_w = (s_grid_w - (ACH_COLS - 1) * gap) / ACH_COLS;
    int tile_h = (s_grid_h - (ACH_ROWS - 1) * gap) / ACH_ROWS;

    for (int slot = 0; slot < ACH_PER_PAGE; slot++) {
        int idx = page * ACH_PER_PAGE + slot;
        if (idx >= ACHIEVEMENTS_COUNT) break;
        const Achievement* a = &ACHIEVEMENTS[idx];
        bool unlocked = achievement_is_unlocked(a->id);
        bool is_quest = (a->id >= WEEKLY_QUEST_BASE && a->id == active_quest);

        int c = slot % ACH_COLS, r = slot / ACH_COLS;
        int x = c * (tile_w + gap), y = r * (tile_h + gap);

        lv_obj_t* tile = lv_obj_create(s_grid_obj);
        lv_obj_remove_style_all(tile);
        lv_obj_set_pos(tile, x, y);
        lv_obj_set_size(tile, tile_w, tile_h);
        lv_obj_set_style_bg_color(tile, THEME_PANEL, 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(tile, 12, 0);
        lv_obj_set_style_border_width(tile, (unlocked || is_quest) ? 2 : 0, 0);
        lv_obj_set_style_border_color(tile, is_quest ? THEME_AMBER : THEME_ACCENT, 0);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tile, ach_tile_tap_cb, LV_EVENT_SHORT_CLICKED,
                            (void*)(intptr_t)idx);
        // Centered flex column: icon -> name -> status (matches the menu tiles).
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(tile, SP_XS, 0);
        lv_obj_set_style_pad_row(tile, SP_XS, 0);

        lv_obj_t* icon = lv_image_create(tile);
        lv_image_set_src(icon, unlocked ? &s_trophy_dsc : &s_lock_dsc);
        lv_obj_set_style_image_recolor(icon,
            unlocked ? THEME_ACCENT : (is_quest ? THEME_AMBER : THEME_DIM), 0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* nm = lv_label_create(tile);
        lv_obj_set_style_text_font(nm, theme_font_body(), 0);
        lv_obj_set_style_text_color(nm,
            unlocked ? THEME_TEXT : (is_quest ? THEME_AMBER : THEME_DIM), 0);
        lv_obj_set_width(nm, tile_w - 2 * SP_XS);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(nm, unlocked ? a->name : "??????");

        lv_obj_t* st = lv_label_create(tile);
        lv_obj_set_style_text_font(st, theme_font_label(), 0);
        lv_obj_set_style_text_color(st, THEME_AMBER, 0);
        lv_obj_set_width(st, tile_w - 2 * SP_XS);
        lv_label_set_long_mode(st, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(st, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(st, is_quest ? "Weekly quest" : "");
    }
}

static void ach_on_page_change(int page) { ach_render_page(page); }

// ---- public API ----

void achievements_screen_init(lv_obj_t* parent) {
    if (s_root) return;

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

    s_header_lbl = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_header_lbl, theme_font_label(), 0);
    lv_obj_set_style_text_color(s_header_lbl, THEME_DIM, 0);
    lv_label_set_text(s_header_lbl, "0 of 0 unlocked");
    lv_obj_align(s_header_lbl, LV_ALIGN_TOP_MID, 0, sbh + SP_S);

    // Inset past the page-arrow gutters (52px) like the menu grid, so tiles
    // don't overlap the "<"/">" nav arrows.
    int gutter_w = 52;
    int list_top = sbh + (large ? 48 : 40);
    s_grid_w = caps.width - 2 * (gutter_w + SP_XS);
    s_grid_h = caps.height - list_top - 36;   // leave room for the dot row

    s_grid_obj = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_grid_obj);
    lv_obj_set_pos(s_grid_obj, gutter_w + SP_XS, list_top);
    lv_obj_set_size(s_grid_obj, s_grid_w, s_grid_h);
    lv_obj_clear_flag(s_grid_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_grid_obj, LV_OPA_TRANSP, 0);

    ach_init_icons();
    paged_nav_create(&s_nav, s_root, 52, list_top, 36, ach_on_page_change);
    int pages = (ACHIEVEMENTS_COUNT + ACH_PER_PAGE - 1) / ACH_PER_PAGE;
    paged_nav_set_page_count(&s_nav, pages);

    Serial.printf("[ach_screen] init done (%d pages), free_heap=%u\n",
                  pages, (unsigned)ESP.getFreeHeap());
}

void achievements_screen_show(void) {
    if (!s_root) return;
    refresh_header();
    paged_nav_goto(&s_nav, 0);   // fires ach_on_page_change -> builds page 0
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    ui_status_bar_set_title("ACHIEVEMENTS");
}

void achievements_screen_hide(void) {
    if (!s_root) return;
    ach_detail_hide();
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}
