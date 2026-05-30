#include "paged_nav.h"
#include "theme.h"
#include "hal/board_caps.h"

#define PN_DOT_SIZE   8
#define PN_DOT_GAP    8

static void pn_left_cb(lv_event_t* e) {
    paged_nav_step((PagedNav*)lv_event_get_user_data(e), -1);
}
static void pn_right_cb(lv_event_t* e) {
    paged_nav_step((PagedNav*)lv_event_get_user_data(e), +1);
}

static lv_obj_t* pn_make_zone(PagedNav* nav, int x, int w, int top, int h,
                              const char* glyph, lv_event_cb_t cb) {
    lv_obj_t* z = lv_obj_create(nav->parent);
    lv_obj_remove_style_all(z);
    lv_obj_set_size(z, w, h);
    lv_obj_set_pos(z, x, top);
    lv_obj_set_style_bg_color(z, THEME_ACCENT, 0);
    lv_obj_set_style_bg_opa(z, LV_OPA_10, 0);
    lv_obj_set_style_radius(z, 14, 0);
    lv_obj_clear_flag(z, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(z, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(z, cb, LV_EVENT_SHORT_CLICKED, nav);

    lv_obj_t* l = lv_label_create(z);
    lv_label_set_text(l, glyph);
    lv_obj_set_style_text_font(l, theme_font_display_s(), 0);
    lv_obj_set_style_text_color(l, THEME_ACCENT, 0);
    lv_obj_center(l);
    return z;
}

void paged_nav_create(PagedNav* nav, lv_obj_t* parent, int gutter_w,
                      int top, int bottom_inset, void (*on_change)(int)) {
    const BoardCaps& caps = board_caps();
    nav->parent     = parent;
    nav->page       = 0;
    nav->page_count = 1;
    nav->on_change  = on_change;

    int zone_h = caps.height - top - bottom_inset;
    nav->left_zone  = pn_make_zone(nav, 0, gutter_w, top, zone_h, "<", pn_left_cb);
    nav->right_zone = pn_make_zone(nav, caps.width - gutter_w, gutter_w, top,
                                   zone_h, ">", pn_right_cb);

    nav->dots_row = lv_obj_create(parent);
    lv_obj_remove_style_all(nav->dots_row);
    lv_obj_set_height(nav->dots_row, PN_DOT_SIZE + 4);
    lv_obj_set_width(nav->dots_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(nav->dots_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav->dots_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(nav->dots_row, PN_DOT_GAP, 0);
    lv_obj_clear_flag(nav->dots_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(nav->dots_row, LV_ALIGN_BOTTOM_MID, 0, -SP_S);
}

static void pn_recolor_dots(PagedNav* nav) {
    uint32_t n = lv_obj_get_child_count(nav->dots_row);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* d = lv_obj_get_child(nav->dots_row, i);
        lv_obj_set_style_bg_color(d,
            ((int)i == nav->page) ? THEME_ACCENT : THEME_BAR_BG, 0);
    }
}

void paged_nav_set_page_count(PagedNav* nav, int count) {
    if (count < 1) count = 1;
    nav->page_count = count;
    if (nav->page >= count) nav->page = count - 1;

    lv_obj_clean(nav->dots_row);
    for (int i = 0; i < count; i++) {
        lv_obj_t* d = lv_obj_create(nav->dots_row);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, PN_DOT_SIZE, PN_DOT_SIZE);
        lv_obj_set_style_radius(d, PN_DOT_SIZE / 2, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE);
    }
    pn_recolor_dots(nav);

    bool single = (count <= 1);
    if (single) {
        lv_obj_add_flag(nav->left_zone,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(nav->right_zone, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(nav->dots_row,   LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(nav->left_zone,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(nav->right_zone, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(nav->dots_row,   LV_OBJ_FLAG_HIDDEN);
    }
}

void paged_nav_goto(PagedNav* nav, int page) {
    if (page < 0) page = 0;
    if (page >= nav->page_count) page = nav->page_count - 1;
    nav->page = page;
    pn_recolor_dots(nav);
    if (nav->on_change) nav->on_change(page);
}

void paged_nav_step(PagedNav* nav, int delta) {
    int p = ((nav->page + delta) % nav->page_count + nav->page_count) % nav->page_count;
    paged_nav_goto(nav, p);
}
