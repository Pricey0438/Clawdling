#include "ui.h"
#include "splash.h"
#include "session.h"
#include "pet_screen.h"
#include "menu_screen.h"
#include "catalog_screen.h"
#include "achievements_screen.h"
#include "retirement_screen.h"
#include "pet_stats_screen.h"
#include "oobe_screen.h"
#include "keyboard_overlay.h"
#include "idle.h"
#include <lvgl.h>
#include <climits>
#include "icons.h"
#include "hal/board_caps.h"
#include "theme.h"
#include "qrcode.h"
#include "net/provisioning.h"
#include "net/config_store.h"
#include "net/net_daemon.h"
#include <esp_heap_caps.h>

// Custom fonts not yet promoted to a theme role — kept as raw declares.
// font_styrene_48 is also declared in theme.h (theme_font_status_xl()); the
// redundant declare here is harmless and preserved per SKIP-row convention.
// font_mono_32 has no accessor (one-off splash animation glyph label below).
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_mono_32);

// Layout values computed from the active board's geometry. Populated once
// in ui_init() and treated as const for the rest of the program. Adding a
// new display size means extending compute_layout() with another
// breakpoint — never editing the screen-builder functions below.
struct Layout {
    int16_t scr_w, scr_h;
    int16_t margin;
    int16_t title_y;
    int16_t content_y;
    int16_t content_w;

    // Usage screen
    int16_t usage_panel_h;
    int16_t usage_panel_gap;
    int16_t usage_bar_y;
    int16_t usage_reset_y;

    // Bluetooth screen
    int16_t bt_info_panel_h;
    int16_t bt_reset_zone_h;
    const lv_font_t* bt_title_font;
    const lv_font_t* bt_status_font;
    const lv_font_t* bt_device_font;
    const lv_font_t* bt_credit_1_font;
    const lv_font_t* bt_credit_2_font;
};
static Layout L = {};

// Pick layout values from the active board's pixel dimensions. The two
// existing boards happen to land on the two breakpoints below; new ports
// inherit the closer one — visually OK, may need a polish pass for
// pixel-perfect alignment but never blocks the port from booting.
static void compute_layout(const BoardCaps& c) {
    L.scr_w = c.width;
    L.scr_h = c.height;
    L.margin = 20;
    L.title_y = 30;

    if (c.height >= 460) {
        // Large layout — tuned for 480x480 (AMOLED-2.16).
        L.content_y = 100;
        L.usage_panel_h = 150;
        L.usage_panel_gap = 16;
        L.usage_bar_y = 56;
        L.usage_reset_y = 94;
        L.bt_info_panel_h = 160;
        L.bt_reset_zone_h = 110;
    } else {
        // Compact layout — tuned for 368x448 (AMOLED-1.8).
        L.content_y = 85;
        L.usage_panel_h = 130;
        L.usage_panel_gap = 12;
        L.usage_bar_y = 48;
        L.usage_reset_y = 78;
        L.bt_info_panel_h = 140;
        L.bt_reset_zone_h = 90;
    }

    // Font slots — accessors encapsulate the H>=460 breakpoint, so the
    // bindings stay out of the branched layout values above.
    L.bt_title_font    = theme_font_display();
    L.bt_status_font   = theme_font_status_xl();
    L.bt_device_font   = theme_font_display_s();
    L.bt_credit_1_font = theme_font_credit_1();
    L.bt_credit_2_font = theme_font_credit_2();

    L.content_w = L.scr_w - 2 * L.margin;
}

// Anthropic brand palette — design tokens live in theme.h (included above).
#include "ui_panels.h"
#include "ui_icons.h"
#define COL_BG        THEME_BG
#define COL_PANEL     THEME_PANEL
#define COL_TEXT      THEME_TEXT
#define COL_DIM       THEME_DIM
#define COL_ACCENT    THEME_ACCENT
#define COL_GREEN     THEME_GREEN
#define COL_AMBER     THEME_AMBER
#define COL_RED       THEME_RED
#define COL_BAR_BG    THEME_BAR_BG

// ---- Usage screen widgets ----
static lv_obj_t* usage_container;
static lv_obj_t* bar_session;
static lv_obj_t* lbl_session_pct;
static lv_obj_t* lbl_session_label;
static lv_obj_t* lbl_session_reset;
static lv_obj_t* bar_weekly;
static lv_obj_t* lbl_weekly_pct;
static lv_obj_t* lbl_weekly_label;
static lv_obj_t* lbl_weekly_reset;
static lv_obj_t* lbl_anim;

// ---- Session screen widgets ----
static lv_obj_t* session_container;

// ---- Pet screen widgets ----
static lv_obj_t* pet_container;

// ---- Catalog screen widgets ----
static lv_obj_t* catalog_container;

// ---- Achievements screen widgets ----
static lv_obj_t* achievements_container;

// ---- Retirement screen widgets ----
static lv_obj_t* retirement_container;

// ---- Stats screen widgets ----
static lv_obj_t* stats_container = nullptr;

// ---- OOBE screen widgets ----
static lv_obj_t* oobe_container = nullptr;

// ---- Bluetooth screen widgets ----
static lv_obj_t* menu_container;
static lv_obj_t* bluetooth_subview;
static lv_obj_t* lbl_ble_status;
static lv_obj_t* lbl_ble_device;
static lv_obj_t* lbl_ble_mac;
static lv_obj_t* reset_lbl_obj = nullptr;     // label inside the Reset zone; switches to "Tap again..." while armed
static uint32_t  reset_arm_ms  = 0;           // 0 = idle, else lv_tick_get() when armed
#define RESET_ARM_WINDOW_MS 3000              // confirm window after the first tap

// ---- Battery indicator + status bar (shared, on top) ----
static lv_obj_t* battery_lbl;   // status-bar battery percentage label
static lv_obj_t* status_bar_obj;     // background panel
static lv_obj_t* status_bar_title;   // left-zone label
static lv_obj_t* status_bar_divider; // 1-px bottom hairline

// ---- Lock state ----
static bool     s_locked          = false;
static screen_t s_pre_lock_screen = SCREEN_SPLASH;
static lv_obj_t* s_lock_pill      = nullptr;   // shown on the top-level screen

// ---- WiFi glyph (left of battery; 3 states: hidden / white / amber) ----
static lv_obj_t*      s_wifi_icon    = nullptr;
static lv_image_dsc_t s_wifi_dsc;    // filled once in update_wifi_glyph()

// ---- Shared ----
static screen_t current_screen = SCREEN_USAGE;

// Animation state
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;
static uint32_t anim_msg_start = 0;
#define ANIM_MSG_MS     4000

static const char* const spinner_frames[] = {
    "\xC2\xB7", "\xE2\x9C\xBB", "\xE2\x9C\xBD",
    "\xE2\x9C\xB6", "\xE2\x9C\xB3", "\xE2\x9C\xA2",
};
#define SPINNER_COUNT 6
#define SPINNER_PHASES (2 * (SPINNER_COUNT - 1))  // 10: ping-pong 0..5..0

static const uint16_t spinner_ms[SPINNER_COUNT] = {
    260, 130, 130, 130, 130, 260,
};

static const char* const anim_messages[] = {
    "Accomplishing", "Elucidating", "Perusing",
    "Actioning", "Enchanting", "Philosophising",
    "Actualizing", "Envisioning", "Pondering",
    "Baking", "Finagling", "Pontificating",
    "Booping", "Flibbertigibbeting", "Processing",
    "Brewing", "Forging", "Puttering",
    "Calculating", "Forming", "Puzzling",
    "Cerebrating", "Frolicking", "Reticulating",
    "Channelling", "Generating", "Ruminating",
    "Churning", "Germinating", "Scheming",
    "Clauding", "Hatching", "Schlepping",
    "Coalescing", "Herding", "Shimmying",
    "Cogitating", "Honking", "Shucking",
    "Combobulating", "Hustling", "Simmering",
    "Computing", "Ideating", "Smooshing",
    "Concocting", "Imagining", "Spelunking",
    "Conjuring", "Incubating", "Spinning",
    "Considering", "Inferring", "Stewing",
    "Contemplating", "Jiving", "Sussing",
    "Cooking", "Manifesting", "Synthesizing",
    "Crafting", "Marinating", "Thinking",
    "Creating", "Meandering", "Tinkering",
    "Crunching", "Moseying", "Transmuting",
    "Deciphering", "Mulling", "Unfurling",
    "Deliberating", "Mustering", "Unravelling",
    "Determining", "Musing", "Vibing",
    "Discombobulating", "Noodling", "Wandering",
    "Divining", "Percolating", "Whirring",
    "Doing", "Wibbling",
    "Effecting", "Wizarding",
    "Working", "Wrangling",
};
#define ANIM_MSG_COUNT (sizeof(anim_messages) / sizeof(anim_messages[0]))

static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

static void format_reset_time(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "Resets in %dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "Resets in %dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "Resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

// Forward decls — callbacks defined near ui_show_screen below
static void global_click_cb(lv_event_t* e);
static void ble_reset_click_cb(lv_event_t* e);

lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, SP_L, 0);
    lv_obj_set_style_pad_right(panel, SP_L, 0);
    lv_obj_set_style_pad_top(panel, SP_M, 0);
    lv_obj_set_style_pad_bottom(panel, SP_M, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    return panel;
}

lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    // Bars never want their own click handling — lv_bar_create leaves
    // CLICKABLE on (inherited from lv_obj), so without this taps on the
    // bar absorb instead of bubbling to the card's row handler.
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    return bar;
}

static lv_obj_t* make_pill(lv_obj_t* parent, const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, theme_font_display_s(), 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl, 18, 0);
    lv_obj_set_style_pad_right(lbl, 18, 0);
    lv_obj_set_style_pad_top(lbl, 6, 0);
    lv_obj_set_style_pad_bottom(lbl, 6, 0);
    return lbl;
}

// update_wifi_glyph — call whenever WiFi / freshness state may have changed.
// parent must be status_bar_obj; the icon is lazy-created on first show.
static void update_wifi_glyph(lv_obj_t* parent) {
    if (!cfg::has_wifi()) {
        if (s_wifi_icon) lv_obj_add_flag(s_wifi_icon, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    // Lazy-create the image object the first time WiFi is configured.
    if (!s_wifi_icon) {
        ui_icon_init_rgb565a8(&s_wifi_dsc, ICON_WIFI_W, ICON_WIFI_H, icon_wifi_data);
        s_wifi_icon = lv_image_create(parent);
        lv_image_set_src(s_wifi_icon, &s_wifi_dsc);
        // Position left of the battery % label. battery_lbl sits at
        // RIGHT_MID − sb_inset; the wifi icon goes just left of it (4px gap).
        lv_obj_align_to(s_wifi_icon, battery_lbl, LV_ALIGN_OUT_LEFT_MID, -(4), 0);
    }
    lv_obj_clear_flag(s_wifi_icon, LV_OBJ_FLAG_HIDDEN);

    // Change-guard the tint — LVGL 9 marks the obj dirty on every style
    // write even if the value is identical, so we'd repaint the glyph
    // every loop tick. Mirror the battery icon's last_pct pattern.
    static uint32_t last_tint = 0xDEADBEEF;
    uint32_t tint_hex = net_daemon::state_fresh() ? 0xFFFFFFu : 0xF5A623u;
    if (tint_hex != last_tint) {
        lv_color_t tint = lv_color_hex(tint_hex);
        lv_obj_set_style_image_recolor(s_wifi_icon, tint, 0);
        lv_obj_set_style_image_recolor_opa(s_wifi_icon, LV_OPA_COVER, 0);
        last_tint = tint_hex;
    }
}

// ======== Usage Screen ========

void make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                             lv_obj_t** out_pct, lv_obj_t** out_pill,
                             lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, L.margin, y, L.content_w, L.usage_panel_h);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, theme_font_display(), 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, L.usage_bar_y, L.content_w - 32, 24);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, theme_font_display_s(), 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, L.usage_reset_y);
}

void make_stat_panel(lv_obj_t* parent, int y, int h,
                     const lv_image_dsc_t* icon,
                     const char* pill_text,
                     lv_obj_t** out_value,
                     lv_obj_t** out_pill,
                     lv_obj_t** out_bar,
                     lv_obj_t** out_subtitle) {
    lv_obj_t* panel = make_panel(parent, L.margin, y, L.content_w, h);

    // Icon top-left. Taps must bubble to the panel so parent rows
    // can use the whole card as a tap target.
    lv_obj_t* icon_img = lv_image_create(panel);
    lv_image_set_src(icon_img, icon);
    lv_obj_align(icon_img, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_clear_flag(icon_img, LV_OBJ_FLAG_CLICKABLE);

    // Pill top-right — same position USAGE puts its "Current"/"Weekly" label.
    *out_pill = make_pill(panel, pill_text);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    // Big numeric value — left-aligned, offset right of icon so the two
    // sit side-by-side. Width naturally floats; pill on the right won't
    // collide because typical values are short ("80", "100", "MAX").
    *out_value = lv_label_create(panel);
    lv_label_set_text(*out_value, "--");
    lv_obj_set_style_text_font(*out_value, theme_font_display(), 0);
    lv_obj_set_style_text_color(*out_value, COL_TEXT, 0);
    lv_obj_align(*out_value, LV_ALIGN_TOP_LEFT, 36, 0);

    // Subtitle bottom-left. Body font keeps the line short enough to clear
    // the bar above it once the chained alignment lands.
    *out_subtitle = lv_label_create(panel);
    lv_label_set_text(*out_subtitle, "");
    lv_obj_set_style_text_font(*out_subtitle, theme_font_body(), 0);
    lv_obj_set_style_text_color(*out_subtitle, COL_DIM, 0);
    lv_obj_align(*out_subtitle, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    // Bar — full-width, anchored 8px above the subtitle so positioning
    // adapts to font metrics across the large/compact breakpoint.
    *out_bar = make_bar(panel, 0, 0, L.content_w - 32, 16);
    lv_obj_align_to(*out_bar, *out_subtitle, LV_ALIGN_OUT_TOP_LEFT, 0, -8);
}

static void init_usage_screen(lv_obj_t* scr) {
    usage_container = lv_obj_create(scr);
    lv_obj_set_size(usage_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_container, 0, 0);
    lv_obj_set_style_bg_opa(usage_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_container, 0, 0);
    lv_obj_set_style_pad_all(usage_container, 0, 0);
    lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(usage_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    make_usage_panel(usage_container, L.content_y, "Current",
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset);
    make_usage_panel(usage_container,
                     L.content_y + L.usage_panel_h + L.usage_panel_gap, "Weekly",
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset);

    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, &font_mono_32, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -15);
}

// ======== Bluetooth Screen ========

static void init_menu_screen(lv_obj_t* scr) {
    menu_container = lv_obj_create(scr);
    lv_obj_set_size(menu_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(menu_container, 0, 0);
    lv_obj_set_style_bg_opa(menu_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(menu_container, 0, 0);
    lv_obj_set_style_pad_all(menu_container, 0, 0);
    lv_obj_clear_flag(menu_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(menu_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    // Phase 4 — wrap the existing BLE widget tree in a sub-container so
    // menu_screen.cpp can show/hide it as a unit. The list view, gallery
    // sub-view, and bluetooth_subview are siblings inside menu_container.
    bluetooth_subview = lv_obj_create(menu_container);
    lv_obj_set_size(bluetooth_subview, L.scr_w, L.scr_h);
    lv_obj_set_pos(bluetooth_subview, 0, 0);
    // Opaque so CO5300 partial-render doesn't keep stale pixels from the
    // sibling list_obj when this view is shown. (Matches all other drill
    // sub-views which use LV_OPA_COVER.)
    lv_obj_set_style_bg_color(bluetooth_subview, COL_BG, 0);
    lv_obj_set_style_bg_opa(bluetooth_subview, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bluetooth_subview, 0, 0);
    lv_obj_set_style_pad_all(bluetooth_subview, 0, 0);
    lv_obj_clear_flag(bluetooth_subview, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* p_info = make_panel(bluetooth_subview, L.margin, L.content_y,
                                  L.content_w, L.bt_info_panel_h);

    static lv_image_dsc_t icon_bt_dsc;
    ui_icon_init_rgb565(&icon_bt_dsc, ICON_BLUETOOTH_W, ICON_BLUETOOTH_H, icon_bluetooth_data);

    lv_obj_t* bt_img = lv_image_create(p_info);
    lv_image_set_src(bt_img, &icon_bt_dsc);
    lv_obj_set_pos(bt_img, 0, 0);

    lbl_ble_status = lv_label_create(p_info);
    lv_label_set_text(lbl_ble_status, "Initializing...");
    lv_obj_set_style_text_font(lbl_ble_status, L.bt_status_font, 0);
    lv_obj_set_style_text_color(lbl_ble_status, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_status, 56, 2);

    lbl_ble_device = lv_label_create(p_info);
    lv_label_set_text(lbl_ble_device, "Device: ---");
    lv_obj_set_style_text_font(lbl_ble_device, L.bt_device_font, 0);
    lv_obj_set_style_text_color(lbl_ble_device, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_device, 0, 64);

    lbl_ble_mac = lv_label_create(p_info);
    lv_label_set_text(lbl_ble_mac, "Address: ---");
    lv_obj_set_style_text_font(lbl_ble_mac, L.bt_device_font, 0);
    lv_obj_set_style_text_color(lbl_ble_mac, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_mac, 0, 100);

    int reset_y = L.content_y + L.bt_info_panel_h + 16;
    lv_obj_t* reset_zone = lv_obj_create(bluetooth_subview);
    lv_obj_set_pos(reset_zone, L.margin, reset_y);
    lv_obj_set_size(reset_zone, L.content_w, L.bt_reset_zone_h);
    lv_obj_set_style_bg_color(reset_zone, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(reset_zone, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(reset_zone, 8, 0);
    lv_obj_set_style_border_width(reset_zone, 0, 0);
    lv_obj_set_style_pad_column(reset_zone, 14, 0);
    lv_obj_set_flex_flow(reset_zone, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(reset_zone, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(reset_zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(reset_zone, ble_reset_click_cb, LV_EVENT_CLICKED, NULL);

    static lv_image_dsc_t icon_trash_dsc;
    ui_icon_init_rgb565(&icon_trash_dsc, ICON_TRASH2_W, ICON_TRASH2_H, icon_trash2_data);
    lv_obj_t* trash_img = lv_image_create(reset_zone);
    lv_image_set_src(trash_img, &icon_trash_dsc);

    reset_lbl_obj = lv_label_create(reset_zone);
    lv_label_set_text(reset_lbl_obj, "Reset Bluetooth");
    lv_obj_set_style_text_font(reset_lbl_obj, L.bt_device_font, 0);
    lv_obj_set_style_text_color(reset_lbl_obj, COL_DIM, 0);

    lv_obj_t* lbl_credit = lv_label_create(bluetooth_subview);
    lv_label_set_text(lbl_credit, "Built by @hermannbjorgvin");
    lv_obj_set_style_text_font(lbl_credit, L.bt_credit_1_font, 0);
    lv_obj_set_style_text_color(lbl_credit, COL_DIM, 0);
    lv_obj_align(lbl_credit, LV_ALIGN_BOTTOM_MID, 0, -46);

    lv_obj_t* lbl_credit2 = lv_label_create(bluetooth_subview);
    lv_label_set_text(lbl_credit2, "Clawd animation by @amaanbuilds");
    lv_obj_set_style_text_font(lbl_credit2, L.bt_credit_2_font, 0);
    lv_obj_set_style_text_color(lbl_credit2, COL_DIM, 0);
    lv_obj_align(lbl_credit2, LV_ALIGN_BOTTOM_MID, 0, -20);

    menu_screen_init(menu_container, bluetooth_subview);

    lv_obj_add_flag(menu_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== Session Screen ========

static void init_session_screen(lv_obj_t* scr) {
    session_container = lv_obj_create(scr);
    lv_obj_set_size(session_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(session_container, 0, 0);
    lv_obj_set_style_bg_color(session_container, THEME_BG, 0);
    lv_obj_set_style_bg_opa(session_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(session_container, 0, 0);
    lv_obj_set_style_pad_all(session_container, 0, 0);
    lv_obj_clear_flag(session_container, LV_OBJ_FLAG_SCROLLABLE);
    // Note: unlike usage_container / menu_container, we do NOT register
    // global_click_cb here. The Sessions screen uses taps to select a
    // session row, so tap-to-toggle-splash would conflict with navigation
    // (taps that miss a row would otherwise route to splash). Splash is
    // still reachable from Usage / Bluetooth or via the PWR-cycle.

    // Session module owns the list view widgets inside this container.
    session_screen_init(session_container);

    lv_obj_add_flag(session_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== Pet Screen ========

static void init_pet_screen(lv_obj_t* scr) {
    pet_container = lv_obj_create(scr);
    lv_obj_set_size(pet_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(pet_container, 0, 0);
    lv_obj_set_style_bg_color(pet_container, COL_BG, 0);
    lv_obj_set_style_bg_opa(pet_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pet_container, 0, 0);
    lv_obj_set_style_pad_all(pet_container, 0, 0);
    lv_obj_clear_flag(pet_container, LV_OBJ_FLAG_SCROLLABLE);
    // No global_click_cb here — tap behavior is owned by the pet_screen
    // module (sprite-tap fires the bond/Pet action; on 2-button boards,
    // Sat/Spi rows are also tap-to-commit). Bubbling a tap to splash
    // would conflict with these affordances.

    pet_screen_init(pet_container);

    lv_obj_add_flag(pet_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== Catalog Screen ========

static void init_catalog_screen(lv_obj_t* scr) {
    catalog_container = lv_obj_create(scr);
    lv_obj_set_size(catalog_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(catalog_container, 0, 0);
    lv_obj_set_style_bg_color(catalog_container, COL_BG, 0);
    lv_obj_set_style_bg_opa(catalog_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(catalog_container, 0, 0);
    lv_obj_set_style_pad_all(catalog_container, 0, 0);
    lv_obj_clear_flag(catalog_container, LV_OBJ_FLAG_SCROLLABLE);

    catalog_screen_init(catalog_container);

    lv_obj_add_flag(catalog_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== Achievements Screen ========
// Lazy-init: the container is created at ui_init time, but the LVGL widget
// tree inside it is built the first time ui_show_screen(SCREEN_ACHIEVEMENTS)
// is called. This keeps the widget count off the critical init path.

static void ensure_achievements_screen(void) {
    if (!achievements_container) return;
    achievements_screen_init(achievements_container);
}

// Retirement screen — same lazy-init pattern as achievements.
static bool s_retirement_inited = false;
static void ensure_retirement_screen(void) {
    if (s_retirement_inited || !retirement_container) return;
    s_retirement_inited = true;
    retirement_screen_init(retirement_container);
}

// ======== Public API ========

void ui_init(void) {
    compute_layout(board_caps());

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    init_usage_screen(scr);
    init_session_screen(scr);
    init_pet_screen(scr);
    init_menu_screen(scr);
    init_catalog_screen(scr);
    // Achievements screen is built lazily on first show (see ensure_achievements_screen).
    // We only create the container here.
    achievements_container = lv_obj_create(scr);
    lv_obj_set_size(achievements_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(achievements_container, 0, 0);
    lv_obj_set_style_bg_color(achievements_container, COL_BG, 0);
    lv_obj_set_style_bg_opa(achievements_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(achievements_container, 0, 0);
    lv_obj_set_style_pad_all(achievements_container, 0, 0);
    lv_obj_clear_flag(achievements_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(achievements_container, LV_OBJ_FLAG_HIDDEN);

    // Retirement screen — lazy-init, container only here.
    retirement_container = lv_obj_create(scr);
    lv_obj_set_size(retirement_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(retirement_container, 0, 0);
    lv_obj_set_style_bg_color(retirement_container, COL_BG, 0);
    lv_obj_set_style_bg_opa(retirement_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(retirement_container, 0, 0);
    lv_obj_set_style_pad_all(retirement_container, 0, 0);
    lv_obj_clear_flag(retirement_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(retirement_container, LV_OBJ_FLAG_HIDDEN);

    // Stats screen — container created here; pet_stats_screen_init() builds
    // the widget tree immediately (stub is lightweight).
    stats_container = lv_obj_create(scr);
    lv_obj_remove_style_all(stats_container);
    lv_obj_set_size(stats_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(stats_container, 0, 0);                      // Fix 3 — match sibling convention
    lv_obj_clear_flag(stats_container, LV_OBJ_FLAG_SCROLLABLE); // Fix 1 — touch-drag would scroll otherwise
    lv_obj_add_flag(stats_container, LV_OBJ_FLAG_HIDDEN);
    pet_stats_screen_init(stats_container);

    // OOBE screen — wave 6a. Container + immediate init (stub is lightweight).
    oobe_container = lv_obj_create(scr);
    lv_obj_remove_style_all(oobe_container);
    lv_obj_set_size(oobe_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(oobe_container, 0, 0);
    lv_obj_clear_flag(oobe_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(oobe_container, LV_OBJ_FLAG_HIDDEN);
    oobe_screen_init(oobe_container);

    splash_init(scr);

    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    // ====== Status bar (replaces the floating logo + battery overlay) ======
    int sbh = theme_status_bar_h();
    int sb_y = SP_XS;                                 // drop bar 4px from top edge
    int sb_inset = SP_XL + SP_M;                      // 32px from each side — clears corner curve

    status_bar_obj = lv_obj_create(scr);
    lv_obj_remove_style_all(status_bar_obj);
    lv_obj_set_size(status_bar_obj, L.scr_w, sbh);
    lv_obj_set_pos(status_bar_obj, 0, sb_y);
    lv_obj_set_style_bg_color(status_bar_obj, THEME_BG, 0);
    lv_obj_set_style_bg_opa(status_bar_obj, LV_OPA_COVER, 0);
    lv_obj_clear_flag(status_bar_obj, LV_OBJ_FLAG_SCROLLABLE);

    status_bar_title = lv_label_create(status_bar_obj);
    lv_label_set_text(status_bar_title, "");
    lv_obj_set_style_text_font(status_bar_title, theme_font_display_s(), 0);
    lv_obj_set_style_text_color(status_bar_title, THEME_TEXT, 0);
    lv_obj_align(status_bar_title, LV_ALIGN_LEFT_MID, sb_inset, 0);

    battery_lbl = lv_label_create(status_bar_obj);
    lv_label_set_text(battery_lbl, "--%");
    lv_obj_set_style_text_font(battery_lbl, theme_font_label(), 0);
    lv_obj_set_style_text_color(battery_lbl, THEME_TEXT, 0);
    lv_obj_align(battery_lbl, LV_ALIGN_RIGHT_MID, -sb_inset, 0);

    status_bar_divider = lv_obj_create(scr);
    lv_obj_remove_style_all(status_bar_divider);
    lv_obj_set_size(status_bar_divider, L.scr_w - 2 * sb_inset, 1);
    lv_obj_set_pos(status_bar_divider, sb_inset, sb_y + sbh);
    lv_obj_set_style_bg_color(status_bar_divider, THEME_DIM, 0);
    lv_obj_set_style_bg_opa(status_bar_divider, LV_OPA_30, 0);
}

void ui_update(const UsageData* data) {
    if (!data->valid) return;

    int s_pct = (int)(data->session_pct + 0.5f);

    lv_label_set_text_fmt(lbl_session_pct, "%d%%", s_pct);
    lv_bar_set_value(bar_session, s_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_session, pct_color(data->session_pct), LV_PART_INDICATOR);

    char buf[48];
    format_reset_time(data->session_reset_mins, buf, sizeof(buf));
    lv_label_set_text(lbl_session_reset, buf);

    int w_pct = (int)(data->weekly_pct + 0.5f);
    lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", w_pct);
    lv_bar_set_value(bar_weekly, w_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_weekly, pct_color(data->weekly_pct), LV_PART_INDICATOR);

    format_reset_time(data->weekly_reset_mins, buf, sizeof(buf));
    lv_label_set_text(lbl_weekly_reset, buf);
}

void ui_tick_anim(void) {
    // De-arm the BLE reset confirmation if the user didn't follow up in time.
    // Runs on every tick regardless of current screen so leaving the BLE
    // screen mid-arm also clears the visual state.
    if (reset_arm_ms != 0 && (lv_tick_get() - reset_arm_ms) > RESET_ARM_WINDOW_MS) {
        reset_arm_ms = 0;
        if (reset_lbl_obj) {
            lv_label_set_text(reset_lbl_obj, "Reset Bluetooth");
            lv_obj_set_style_text_color(reset_lbl_obj, COL_DIM, 0);
        }
    }

    if (current_screen == SCREEN_PET) {
        pet_screen_tick();
        return;
    }
    if (current_screen == SCREEN_SESSION) {
        session_screen_tick_anim();
        return;
    }
    if (current_screen == SCREEN_MENU) {
        menu_screen_tick();
        return;
    }
    if (current_screen != SCREEN_USAGE) return;

    uint32_t now = lv_tick_get();

    if (now - anim_msg_start >= ANIM_MSG_MS) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_msg_start = now;
    }

    if (now - anim_last_ms >= spinner_ms[anim_spinner_idx]) {
        anim_last_ms = now;
        anim_phase = (anim_phase + 1) % SPINNER_PHASES;
        anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                        : (SPINNER_PHASES - anim_phase);

        static char buf[80];
        snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
                 spinner_frames[anim_spinner_idx],
                 anim_messages[anim_msg_idx]);
        lv_label_set_text(lbl_anim, buf);
    }
}

static screen_t prev_non_splash_screen = SCREEN_USAGE;
static void apply_status_bar_visibility(void) {
    if (!status_bar_obj) return;
    bool hide = (current_screen == SCREEN_SPLASH);
    if (hide) {
        lv_obj_add_flag(status_bar_obj, LV_OBJ_FLAG_HIDDEN);
        if (status_bar_divider) lv_obj_add_flag(status_bar_divider, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(status_bar_obj, LV_OBJ_FLAG_HIDDEN);
        if (status_bar_divider) lv_obj_clear_flag(status_bar_divider, LV_OBJ_FLAG_HIDDEN);
    }
}

static void global_click_cb(lv_event_t* e) {
    (void)e;
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

static void ble_reset_click_cb(lv_event_t* e) {
    // Stop bubbling defensively — the reset zone is now a child of
    // bluetooth_subview (which is a child of menu_container), and a stray
    // bubble would also fire global_click_cb (splash toggle).
    lv_event_stop_bubbling(e);
    uint32_t now = lv_tick_get();
    if (reset_arm_ms != 0 && (now - reset_arm_ms) < RESET_ARM_WINDOW_MS) {
        // Second tap inside the confirm window → actually wipe bonds.
        reset_arm_ms = 0;
        if (reset_lbl_obj) {
            lv_label_set_text(reset_lbl_obj, "Reset Bluetooth");
            lv_obj_set_style_text_color(reset_lbl_obj, COL_DIM, 0);
        }
        ble_clear_bonds();
    } else {
        // First tap → arm. Reverted by ui_tick_anim if the user doesn't follow up.
        reset_arm_ms = now;
        if (reset_lbl_obj) {
            lv_label_set_text(reset_lbl_obj, "Tap again to confirm");
            lv_obj_set_style_text_color(reset_lbl_obj, COL_AMBER, 0);
        }
    }
}

// ======== Screen transition animations (Approach B — container cross-fade) ========

#define SCREEN_FADE_MS  120u

static void anim_opacity_cb(void* obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t*)obj, (lv_opa_t)v, 0);
}

static void anim_finished_hide_cb(lv_anim_t* a) {
    lv_obj_t* obj = (lv_obj_t*)lv_anim_get_user_data(a);
    if (obj) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    // Restore full opacity so next show-and-fade-in starts from transparent
    if (obj) lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
}

// Fade out an already-visible container then hide it.
static void fade_out_then_hide(lv_obj_t* obj) {
    if (!obj) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, anim_opacity_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, SCREEN_FADE_MS);
    lv_anim_set_user_data(&a, obj);
    lv_anim_set_completed_cb(&a, anim_finished_hide_cb);
    lv_anim_start(&a);
}

// Unhide a container and fade it in from transparent to opaque.
static void show_and_fade_in(lv_obj_t* obj) {
    if (!obj) return;
    lv_obj_set_style_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, anim_opacity_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, SCREEN_FADE_MS);
    lv_anim_start(&a);
}

// Return the root container for a given screen (nullptr for SCREEN_SPLASH —
// splash has its own show/hide but its container is accessible via
// splash_get_root() when needed).
static lv_obj_t* container_for_screen(screen_t s) {
    switch (s) {
        case SCREEN_USAGE:        return usage_container;
        case SCREEN_SESSION:      return session_container;
        case SCREEN_PET:          return pet_container;
        case SCREEN_MENU:         return menu_container;
        case SCREEN_CATALOG:      return catalog_container;
        case SCREEN_ACHIEVEMENTS: return achievements_container;
        case SCREEN_RETIREMENT:   return retirement_container;
        case SCREEN_STATS:        return stats_container;
        case SCREEN_OOBE:         return oobe_container;
        case SCREEN_SPLASH:       return splash_get_root();
        default:                  return nullptr;
    }
}

void ui_show_screen(screen_t screen) {
    uint32_t t_show_start = millis();
    // No-op guard — avoids double-animating the same screen.
    if (screen == current_screen) return;

    // Lock gate — while locked, only SPLASH navigations land. ui_set_locked
    // calls itself through this path to force splash on lock and to restore
    // the prior screen on unlock; both pass through fine because either the
    // target IS splash, or s_locked is already false by the time the unlock
    // path calls here.
    if (s_locked && screen != SCREEN_SPLASH) return;

    // Reset session sub-view (list/detail) when leaving SCREEN_SESSION so a
    // future return drops back to the list rather than the previous detail.
    if (current_screen == SCREEN_SESSION && screen != SCREEN_SESSION) {
        session_screen_reset_view();
    }

    // Phase 4 — free gallery PSRAM thumbnails when leaving SCREEN_MENU.
    // Without this, a background tap from the gallery sub-view routes
    // through ui_toggle_splash → ui_show_screen(SCREEN_SPLASH) and leaks
    // up to 50 × 60×60 RGB565 thumbs (~360 KB) until the menu is re-entered.
    if (current_screen == SCREEN_MENU && screen != SCREEN_MENU) {
        menu_screen_reset_view();
    }

    // Tear down any pet-screen modal (e.g. recap) when leaving SCREEN_PET —
    // modals are parented to lv_scr_act() and otherwise float above the new
    // screen until tap-dismissed (P0-2 / wave-3-pet-feature-qa).
    if (current_screen == SCREEN_PET && screen != SCREEN_PET) {
        pet_screen_hide_modals();
    }
    // Same for retirement's bio-card modal (P0-1).
    if (current_screen == SCREEN_RETIREMENT && screen != SCREEN_RETIREMENT) {
        retirement_screen_hide_modals();
    }

    // Fade out the currently visible container (if any).
    // Containers that are already HIDDEN are left alone — fade_out_then_hide
    // only schedules the animation; nothing breaks if the object was already
    // invisible (LVGL ignores animations on hidden objects gracefully).
    lv_obj_t* outgoing = container_for_screen(current_screen);
    if (outgoing) {
        fade_out_then_hide(outgoing);
    } else {
        // Fallback hard-hide for any screen not covered above.
        splash_hide();
    }

    // Hard-hide all other containers immediately so stale content from a
    // previous rapid switch doesn't bleed through.
    lv_obj_t* all_containers[] = {
        usage_container, session_container, pet_container, menu_container,
        catalog_container, achievements_container, retirement_container,
        stats_container, oobe_container, splash_get_root()
    };
    for (auto* c : all_containers) {
        if (c && c != outgoing) {
            lv_anim_delete(c, anim_opacity_cb);  // cancel any in-flight anim
            lv_obj_set_style_opa(c, LV_OPA_COVER, 0);
            lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
        }
    }
    // splash_hide() keeps its own `active` flag consistent.
    if (current_screen != SCREEN_SPLASH) splash_hide();

    // Pre-init lazy screens before we try to show them.
    if (screen == SCREEN_ACHIEVEMENTS) ensure_achievements_screen();
    if (screen == SCREEN_RETIREMENT)   ensure_retirement_screen();

    // Run any per-screen _show() hooks (title updates, state refresh, etc.)
    // before making the container visible so labels are populated on first frame.
    switch (screen) {
    case SCREEN_USAGE:        ui_status_bar_set_title("USAGE"); break;
    case SCREEN_SESSION:      ui_status_bar_set_title("SESSIONS"); break;
    case SCREEN_PET:          pet_screen_show(); break;
    case SCREEN_MENU:         menu_screen_show(); break;
    case SCREEN_CATALOG:      catalog_screen_show(); break;
    case SCREEN_ACHIEVEMENTS: achievements_screen_show(); break;
    case SCREEN_RETIREMENT:   retirement_screen_show(); break;
    case SCREEN_STATS:        pet_stats_screen_show(); break;
    case SCREEN_OOBE:         oobe_screen_show(); break;
    default: break;
    }

    // Transition timing instrumentation — "pre-fade" = time from entering
    // ui_show_screen until the per-screen _show() hooks finish and we are
    // about to start the fade-in animation.
    uint32_t t_fade_start = millis();
    Serial.printf("[xtime] %s pre-fade=%lums\n",
                  screen == SCREEN_PET     ? "→PET"     :
                  screen == SCREEN_STATS   ? "→STATS"   :
                  screen == SCREEN_USAGE   ? "→USAGE"   :
                  screen == SCREEN_SESSION ? "→SESSION" :
                  screen == SCREEN_MENU    ? "→MENU"    : "→OTHER",
                  (unsigned long)(t_fade_start - t_show_start));

    // Fade in the incoming container.  For SCREEN_SPLASH splash_show()
    // un-hides the container internally, so we call it first then animate.
    lv_obj_t* incoming = container_for_screen(screen);
    if (screen == SCREEN_SPLASH) {
        splash_show();   // sets active=true and clears HIDDEN flag
        if (incoming) {
            lv_obj_set_style_opa(incoming, LV_OPA_TRANSP, 0);
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, incoming);
            lv_anim_set_exec_cb(&a, anim_opacity_cb);
            lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
            lv_anim_set_duration(&a, SCREEN_FADE_MS);
            lv_anim_start(&a);
        }
    } else if (incoming) {
        show_and_fade_in(incoming);
    }

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
    apply_status_bar_visibility();
}

void ui_cycle_screen(void) {
    switch (current_screen) {
        case SCREEN_USAGE:     ui_show_screen(SCREEN_SESSION);   break;
        case SCREEN_SESSION:   ui_show_screen(SCREEN_PET);       break;
        case SCREEN_PET:       ui_show_screen(SCREEN_MENU);   break;
        case SCREEN_STATS:
            // SCREEN_STATS is intentionally outside the cycle (spec §3). If the
            // user somehow lands here on a cycle press, route back to PET — that's
            // the direct-link parent and the back path users expect.
            ui_show_screen(SCREEN_PET);
            break;
        case SCREEN_MENU:      ui_show_screen(SCREEN_USAGE);  break;
        // Menu-drilled detail screens — MID button returns to MENU (the
        // parent the user came from), not USAGE. Without these explicit
        // cases users had no back path on 2-button boards (P1-10 / P1-19
        // / wave-3-pet-feature-qa).
        case SCREEN_CATALOG:      ui_show_screen(SCREEN_MENU); break;
        case SCREEN_ACHIEVEMENTS: ui_show_screen(SCREEN_MENU); break;
        case SCREEN_RETIREMENT:   ui_show_screen(SCREEN_MENU); break;
        // OOBE is a setup flow — a BOOT press while OOBE is showing must not
        // silently skip past setup and dump the user on USAGE. The auto-hide
        // path (data source coming online) is the only exit. No-op here.
        case SCREEN_OOBE:      break;
        default:               ui_show_screen(SCREEN_USAGE);  break;
    }
}

void ui_input_event(ui_btn_t btn, bool pressed) {
    if (!pressed) return;                        // act on press edges only
    if (idle_consume_wake_press()) return;       // first wake-press is swallowed

    // Keyboard overlay (hatch naming, menu rename) is modal — swallow all
    // physical button presses while it's up so MID can't cycle the underlying
    // screen out from under the overlay (P1-2 / wave-3-pet-feature-qa).
    // LVGL keyboard taps still reach the textarea via the touch dispatcher.
    if (kbd_overlay_is_shown()) return;

    // Per-screen handlers. SESSION, PET, and MENU each own their button routing.
    bool consumed = false;
    switch (current_screen) {
        case SCREEN_SESSION: consumed = session_on_button(btn); break;
        case SCREEN_PET:
            if (btn == UI_BTN_RIGHT && board_caps().button_count >= 2) {
                // On 3-button boards (button_count >= 2 = primary + secondary),
                // the right button on the pet screen routes to SCREEN_STATS instead
                // of running the per-screen default. STATS is intentionally outside
                // the screen cycle per spec §3 — this is the direct-link entry.
                ui_show_screen(SCREEN_STATS);
                consumed = true;
                break;
            }
            consumed = pet_screen_on_button(btn);
            break;
        case SCREEN_MENU:    consumed = menu_screen_on_button(btn); break;
        case SCREEN_STATS:   consumed = pet_stats_screen_on_button(btn); break;
        default:             break;
    }
    if (consumed) return;

    if (btn == UI_BTN_MID) {
        if (current_screen == SCREEN_SPLASH) splash_next();
        else                                  ui_cycle_screen();
    }
}

void ui_toggle_splash(void) {
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

// ---- Lock ----

static void build_lock_pill_if_needed(void) {
    if (s_lock_pill) return;
    // Parent the pill to LV_LAYER_TOP so it floats above every screen
    // regardless of which screen is active — survives screen switches
    // without re-parenting and can't be intercepted by screen-level widgets.
    lv_obj_t* parent = lv_layer_top();
    if (!parent) return;
    s_lock_pill = lv_label_create(parent);
    lv_label_set_text(s_lock_pill, "[ LOCKED ]");
    lv_obj_set_style_text_font(s_lock_pill, theme_font_label(), 0);
    lv_obj_set_style_text_color(s_lock_pill, THEME_TEXT, 0);
    lv_obj_set_style_bg_color(s_lock_pill, THEME_PANEL, 0);
    lv_obj_set_style_bg_opa(s_lock_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(s_lock_pill, SP_M, 0);
    lv_obj_set_style_pad_right(s_lock_pill, SP_M, 0);
    lv_obj_set_style_pad_top(s_lock_pill, SP_XS, 0);
    lv_obj_set_style_pad_bottom(s_lock_pill, SP_XS, 0);
    lv_obj_set_style_radius(s_lock_pill, 12, 0);
    lv_obj_align(s_lock_pill, LV_ALIGN_TOP_MID, 0, theme_status_bar_h() + SP_M);
    // The pill is on the top screen so make sure it draws above splash widgets.
    lv_obj_move_foreground(s_lock_pill);
    lv_obj_add_flag(s_lock_pill, LV_OBJ_FLAG_HIDDEN);
}

void ui_set_locked(bool locked) {
    if (locked == s_locked) return;
    s_locked = locked;
    // Fast-sleep: 30s timeout while locked + ignore USB exemption, so a
    // locked-and-shelved device powers down its panel quickly. Restored
    // to the normal 30-min timeout on unlock.
    idle_set_fast_sleep(locked);
    if (locked) {
        s_pre_lock_screen = current_screen;
        ui_show_screen(SCREEN_SPLASH);
        build_lock_pill_if_needed();
        if (s_lock_pill) {
            lv_obj_move_foreground(s_lock_pill);
            lv_obj_clear_flag(s_lock_pill, LV_OBJ_FLAG_HIDDEN);
        }
        Serial.println("[lock] locked");
    } else {
        if (s_lock_pill) lv_obj_add_flag(s_lock_pill, LV_OBJ_FLAG_HIDDEN);
        ui_show_screen(s_pre_lock_screen);
        Serial.println("[lock] unlocked");
    }
}

bool ui_is_locked(void) {
    return s_locked;
}

void ui_toggle_lock(void) {
    ui_set_locked(!s_locked);
}

static void (*status_bar_back_cb)(void) = nullptr;

static void status_bar_title_click_cb(lv_event_t* e) {
    (void)e;
    void (*cb)(void) = status_bar_back_cb;
    if (cb) cb();   // local copy in case cb clears the slot on invoke
}

void ui_status_bar_set_title(const char* text) {
    if (!status_bar_title) return;
    lv_label_set_text(status_bar_title, text ? text : "");
}

void ui_status_bar_set_back_cb(void (*cb)(void)) {
    if (!status_bar_title) return;
    if (cb && !status_bar_back_cb) {
        // First-time bind — install click handler + add visible affordance.
        lv_obj_add_flag(status_bar_title, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(status_bar_title, status_bar_title_click_cb,
                            LV_EVENT_CLICKED, NULL);
        lv_obj_set_style_bg_color(status_bar_title, THEME_PANEL, 0);
        lv_obj_set_style_bg_opa(status_bar_title, LV_OPA_30, 0);
        lv_obj_set_style_pad_left(status_bar_title, 6, 0);
        lv_obj_set_style_pad_right(status_bar_title, 6, 0);
        lv_obj_set_style_radius(status_bar_title, 4, 0);
    } else if (!cb && status_bar_back_cb) {
        // Unbind — clear flag and the affordance styling. LVGL 9 has no
        // lv_obj_remove_event_cb, so the click handler stays registered for
        // the title's lifetime. That's fine: status_bar_title_click_cb early-
        // returns when status_bar_back_cb is null, AND clearing the
        // CLICKABLE flag stops LVGL from dispatching clicks to it at all.
        lv_obj_clear_flag(status_bar_title, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(status_bar_title, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_left(status_bar_title, 0, 0);
        lv_obj_set_style_pad_right(status_bar_title, 0, 0);
    }
    status_bar_back_cb = cb;
}

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    switch (state) {
    case BLE_STATE_CONNECTED:
        lv_label_set_text(lbl_ble_status, "Connected");
        lv_obj_set_style_text_color(lbl_ble_status, COL_GREEN, 0);
        break;
    case BLE_STATE_ADVERTISING:
        lv_label_set_text(lbl_ble_status, "Advertising...");
        lv_obj_set_style_text_color(lbl_ble_status, COL_AMBER, 0);
        break;
    case BLE_STATE_DISCONNECTED:
        lv_label_set_text(lbl_ble_status, "Disconnected");
        lv_obj_set_style_text_color(lbl_ble_status, COL_RED, 0);
        break;
    default:
        lv_label_set_text(lbl_ble_status, "Initializing...");
        lv_obj_set_style_text_color(lbl_ble_status, COL_DIM, 0);
        break;
    }

    if (name) {
        static char nbuf[48];
        snprintf(nbuf, sizeof(nbuf), "Device: %s", name);
        lv_label_set_text(lbl_ble_device, nbuf);
    }
    if (mac) {
        static char mbuf[48];
        snprintf(mbuf, sizeof(mbuf), "Address: %s", mac);
        lv_label_set_text(lbl_ble_mac, mbuf);
    }

    // Phase 4 — keep the menu list's "Bluetooth — <state>" secondary in sync.
    menu_screen_refresh_secondary_labels();
}

void ui_update_battery(int percent, bool charging) {
    if (!battery_lbl) return;

    // Change-guard: only rewrite text/color when the displayed state changes,
    // so we don't dirty the label every loop tick (mirrors the wifi last_tint
    // and old last_pct patterns).
    static int  last_pct      = INT_MIN;
    static bool last_charging = false;
    if (percent == last_pct && charging == last_charging) return;
    last_pct      = percent;
    last_charging = charging;

    char buf[8];
    if (percent < 0) snprintf(buf, sizeof(buf), "USB");
    else             snprintf(buf, sizeof(buf), "%d%%", percent);
    lv_label_set_text(battery_lbl, buf);

    lv_obj_set_style_text_color(battery_lbl,
                                charging ? THEME_GREEN : THEME_TEXT, 0);

    apply_status_bar_visibility();
}

void ui_update_wifi(void) {
    if (!status_bar_obj) return;
    update_wifi_glyph(status_bar_obj);
}

// ======== Provisioning Screen ========

void ui_show_provisioning(const String& ap_ssid) {
    // WiFi-join QR payload — phones treat it as "tap to join this open network"
    String payload = "WIFI:T:nopass;S:" + ap_ssid + ";;";

    // Version 3 (29×29) handles up to 53 alphanumeric chars at ECC_LOW;
    // "WIFI:T:nopass;S:Clawdmeter-XXXX;;" is ~34 chars — comfortable fit.
    // qrcode_getBufferSize() is a runtime function, not constexpr, so we
    // use a fixed-size array. Version 3 requires ~130 bytes; 200 is ample.
    QRCode qr;
    static uint8_t qr_buf[200];
    qrcode_initText(&qr, qr_buf, 3, ECC_LOW, payload.c_str());

    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, THEME_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "Connect to set up");
    lv_obj_set_style_text_font(title, theme_font_body(), 0);
    lv_obj_set_style_text_color(title, THEME_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_t* ssid_lbl = lv_label_create(scr);
    lv_label_set_text(ssid_lbl, ap_ssid.c_str());
    lv_obj_set_style_text_font(ssid_lbl, theme_font_display_s(), 0);
    lv_obj_set_style_text_color(ssid_lbl, THEME_TEXT, 0);
    lv_obj_align(ssid_lbl, LV_ALIGN_TOP_MID, 0, 40);

    // Render the QR code directly into a uint16_t pixel buffer (PSRAM),
    // identical approach to splash.cpp — avoids the unavailable
    // lv_canvas_fill_area() and is faster than per-pixel lv_canvas_set_px().
    const BoardCaps& caps = board_caps();
    int smaller = (caps.width < caps.height) ? caps.width : caps.height;
    int cell = (smaller - 140) / (int)qr.size;
    if (cell < 2) cell = 2;
    int px = qr.size * cell;

    size_t buf_bytes = (size_t)px * px * sizeof(uint16_t);
    uint16_t* cbuf = (uint16_t*)heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    if (!cbuf) {
        Serial.printf("ui_prov: QR canvas alloc failed (%u bytes)\n", (unsigned)buf_bytes);
        lv_scr_load(scr);
        return;
    }

    // Fill white background
    const uint16_t WHITE_565 = 0xFFFF;
    const uint16_t BLACK_565 = 0x0000;
    for (int i = 0; i < px * px; i++) cbuf[i] = WHITE_565;

    // Paint dark modules
    for (uint8_t gy = 0; gy < qr.size; gy++) {
        for (uint8_t gx = 0; gx < qr.size; gx++) {
            if (qrcode_getModule(&qr, gx, gy)) {
                for (int dy = 0; dy < cell; dy++) {
                    uint16_t* row = &cbuf[(gy * cell + dy) * px + gx * cell];
                    for (int dx = 0; dx < cell; dx++) row[dx] = BLACK_565;
                }
            }
        }
    }

    lv_obj_t* canvas = lv_canvas_create(scr);
    if (!canvas) {
        // Under extreme PSRAM pressure lv_obj_create can fail. cbuf hasn't
        // been handed to a widget yet, so we own it and must free it.
        Serial.println("ui_prov: lv_canvas_create failed; freeing QR buffer");
        heap_caps_free(cbuf);
        lv_scr_load(scr);
        return;
    }
    // Ownership of cbuf passes to the canvas widget here. The canvas (and the
    // screen it lives on) persists until reboot, so cbuf is intentionally
    // leaked — there is no cleanup path that destroys this screen.
    lv_canvas_set_buffer(canvas, cbuf, px, px, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(canvas, LV_ALIGN_CENTER, 0, 20);

    lv_obj_t* hint = lv_label_create(scr);
    lv_label_set_text(hint, "or visit http://192.168.4.1");
    lv_obj_set_style_text_font(hint, theme_font_body(), 0);
    lv_obj_set_style_text_color(hint, THEME_TEXT, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);

    lv_scr_load(scr);
}
