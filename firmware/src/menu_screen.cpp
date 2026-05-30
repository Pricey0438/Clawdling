#include "menu_screen.h"
#include "version.h"
#include "theme.h"
#include "ui.h"
#include "pet.h"      // for pet_gallery() to read graduate count
#include "catalog_screen.h"
#include "achievements.h"
#include "keyboard_overlay.h"   // for Rename Pet
#include "ble.h"      // for ble_get_state()/name()/mac_address()
#include "radio.h"
#include "net/net_daemon.h"
#include "net/config_store.h"
#include "settings.h"
#include "hal/display_hal.h"
#include "splash.h"   // for splash_render_to_buf (pet-info hero sprite)
#include "hal/board_caps.h"
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include "icons.h"
#include "ui_icons.h"
#include "paged_nav.h"

extern "C" uint32_t app_last_apply_ms(void);

// Icon descriptors. icon_bluetooth_data is RGB565 (legacy uint16_t);
// the rest are RGB565A8 (planar 48×48 Lucide glyphs).
static lv_image_dsc_t icon_bluetooth_dsc;
static lv_image_dsc_t icon_images_dsc;
static lv_image_dsc_t icon_paw_dsc;
static lv_image_dsc_t icon_grid_dsc;
static lv_image_dsc_t icon_refresh_dsc;
static lv_image_dsc_t icon_sun_dsc;
static lv_image_dsc_t icon_wifi_lg_dsc;
static lv_image_dsc_t icon_info_dsc;
static lv_image_dsc_t icon_alert_dsc;

static void init_menu_icons(void) {
    ui_icon_init_rgb565  (&icon_bluetooth_dsc, ICON_BLUETOOTH_W, ICON_BLUETOOTH_H, icon_bluetooth_data);
    ui_icon_init_rgb565a8(&icon_images_dsc,    ICON_IMAGES_W,    ICON_IMAGES_H,    icon_images_data);
    ui_icon_init_rgb565a8(&icon_paw_dsc,       ICON_PAW_W,       ICON_PAW_H,       icon_paw_data);
    ui_icon_init_rgb565a8(&icon_grid_dsc,      ICON_GRID_W,      ICON_GRID_H,      icon_grid_data);
    ui_icon_init_rgb565a8(&icon_refresh_dsc,   ICON_REFRESH_W,   ICON_REFRESH_H,   icon_refresh_data);
    ui_icon_init_rgb565a8(&icon_sun_dsc,       ICON_SUN_W,       ICON_SUN_H,       icon_sun_data);
    ui_icon_init_rgb565a8(&icon_wifi_lg_dsc,   ICON_WIFI_LG_W,   ICON_WIFI_LG_H,   icon_wifi_lg_data);
    ui_icon_init_rgb565a8(&icon_info_dsc,      ICON_INFO_W,      ICON_INFO_H,      icon_info_data);
    ui_icon_init_rgb565a8(&icon_alert_dsc,     ICON_ALERT_W,     ICON_ALERT_H,     icon_alert_data);
}

// View state.
enum menu_view_t { MENU_VIEW_LIST, MENU_VIEW_BLUETOOTH, MENU_VIEW_ABOUT, MENU_VIEW_BRIGHTNESS, MENU_VIEW_PETINFO, MENU_VIEW_SPLASHPICK, MENU_VIEW_WIFI, MENU_VIEW_FRESET };
static menu_view_t current_view = MENU_VIEW_LIST;

// External handles from ui.cpp.
static lv_obj_t* root_obj = nullptr;
static lv_obj_t* bt_subview = nullptr;

// Paged icon-grid (replaces the old scroll list). 2 columns x 3 rows per page.
static lv_obj_t* grid_obj = nullptr;     // the container that holds one page of tiles
#define MENU_ITEM_MAX     20
#define GRID_COLS         2
#define GRID_ROWS         3
#define GRID_PAGE_TILES   (GRID_COLS * GRID_ROWS)   // 6
static lv_obj_t* tile_obj[GRID_PAGE_TILES]      = {0};  // reused across pages
static lv_obj_t* tile_icon[GRID_PAGE_TILES]     = {0};
static lv_obj_t* tile_primary[GRID_PAGE_TILES]  = {0};
static lv_obj_t* tile_status[GRID_PAGE_TILES]   = {0};
static int       item_count = 0;
static int       menu_cursor = 0;        // index into MENU_ITEMS (global, across pages)
static PagedNav  grid_nav;

// About sub-view (Phase 1).
static lv_obj_t* about_obj = nullptr;
static lv_obj_t* brightness_obj = nullptr;
static lv_obj_t* brightness_pills[3] = {nullptr, nullptr, nullptr};
static lv_obj_t* petinfo_obj          = nullptr;
static lv_obj_t* petinfo_hero_name    = nullptr;
static lv_obj_t* petinfo_hero_sub     = nullptr;   // "Lv N - stage"
static lv_obj_t* petinfo_hero_canvas  = nullptr;
static uint16_t* petinfo_sprite_buf   = nullptr;   // PSRAM sprite buffer
static lv_obj_t* petinfo_val_species  = nullptr;
static lv_obj_t* petinfo_val_born     = nullptr;
static lv_obj_t* petinfo_val_total    = nullptr;
static lv_obj_t* petinfo_val_tonext   = nullptr;
static lv_obj_t* petinfo_val_evolve   = nullptr;
#define PETINFO_SPRITE_CELL  4            /* 80x80 sprite */
#define PETINFO_SPRITE_PX    (20 * PETINFO_SPRITE_CELL)
// Splash picker — single-preview carousel. browse_idx walks 0..13 where
// position 0 is "Auto" (rotating species preview) and 1..13 map to
// species 0..12. The splash pin is only written when the user confirms
// via the Set button — browsing alone doesn't change persistence.
static lv_obj_t* splash_pick_obj      = nullptr;
static lv_obj_t* sp_canvas            = nullptr;
static lv_obj_t* sp_left_zone         = nullptr;
static lv_obj_t* sp_right_zone        = nullptr;
static lv_obj_t* sp_name_lbl          = nullptr;
static lv_obj_t* sp_counter_lbl       = nullptr;
static lv_obj_t* sp_set_btn           = nullptr;
static lv_obj_t* sp_set_btn_lbl       = nullptr;
static uint16_t* sp_canvas_buf        = nullptr;
static int       sp_browse_idx        = 0;
static int       sp_cell              = 0;        // px per 20-cell grid unit
static int       sp_canvas_px         = 0;        // total canvas dimension (20 * sp_cell)
static uint16_t  sp_frame_idx         = 0;
static uint32_t  sp_last_frame_ms     = 0;
static uint32_t  sp_auto_rotate_ms    = 0;
static uint8_t   sp_auto_species      = 0;
#define SP_FRAME_MS         120     // frame cadence
#define SP_AUTO_ROTATE_MS  2200     // Auto preview cycles species every 2.2s

static lv_obj_t* wifi_obj = nullptr;
static lv_obj_t* wifi_lbl_ssid = nullptr;
static lv_obj_t* wifi_lbl_rssi = nullptr;
static lv_obj_t* wifi_lbl_ip   = nullptr;
static lv_obj_t* wifi_lbl_url  = nullptr;
static lv_obj_t* wifi_lbl_poll = nullptr;
static lv_obj_t* wifi_lbl_action = nullptr;
static lv_obj_t* wifi_sw       = nullptr;   // lv_switch driving radio::set_wifi
static lv_obj_t* wifi_note     = nullptr;   // "turning WiFi on turns BLE off"
static lv_obj_t* wifi_content  = nullptr;   // info rows + actions, toggled as a unit
static lv_obj_t* wifi_offmsg   = nullptr;   // centered off-state message
static lv_obj_t* wifi_card     = nullptr;   // grouped info panel card
static lv_obj_t* wifi_status_dot = nullptr; // colored connection-state dot
static lv_obj_t* wifi_lbl_status = nullptr; // "Connected" / "Disconnected"

static lv_obj_t* bt_obj        = nullptr;   // owned by menu_screen
static lv_obj_t* bt_sw         = nullptr;
static lv_obj_t* bt_lbl_name   = nullptr;
static lv_obj_t* bt_lbl_host   = nullptr;
static lv_obj_t* bt_lbl_mac    = nullptr;
static lv_obj_t* bt_content    = nullptr;   // info rows, toggled as a unit
static lv_obj_t* bt_offmsg     = nullptr;
static lv_obj_t* bt_card       = nullptr;   // grouped info panel card
static lv_obj_t* bt_status_dot = nullptr;   // colored connection-state dot
static lv_obj_t* bt_lbl_conn   = nullptr;   // "Connected"/"Advertising"/etc

enum wifi_action_state_t { WAS_IDLE, WAS_RECONNECTING, WAS_OK, WAS_FAIL };
static wifi_action_state_t wifi_action_state = WAS_IDLE;
static uint32_t            wifi_action_started_ms = 0;
#define WIFI_RECONNECT_TIMEOUT_MS  10000

static lv_obj_t* freset_obj = nullptr;
static lv_obj_t* freset_btn = nullptr;
static lv_obj_t* freset_bar = nullptr;
static uint32_t  freset_press_started_ms = 0;
#define FRESET_HOLD_MS 3000

// MenuItem schema. secondary_fn() returns dynamic right-side text
// (caller mustn't free; returns pointer to a static buffer).
struct MenuItem {
    const char* primary;
    const char* (*secondary_fn)(void);
    void (*drill_fn)(void);
    const lv_image_dsc_t* icon;
};

static const char* bluetooth_secondary(void);
static void drill_bluetooth(void);
static void show_list_view(void);
static void touch_wait_release(void);
static void format_uptime(uint32_t sec, char* buf, size_t len);
void menu_screen_refresh_secondary_labels(void);  // forward decl for vacation_toggle_cb

static const char* about_secondary(void) { return ""; }

static void build_about_view(void) {
    if (about_obj) return;
    const BoardCaps& caps = board_caps();

    about_obj = lv_obj_create(root_obj);
    lv_obj_remove_style_all(about_obj);
    lv_obj_set_size(about_obj, caps.width, caps.height);
    lv_obj_set_style_bg_color(about_obj, THEME_BG, 0);
    lv_obj_set_style_bg_opa(about_obj, LV_OPA_COVER, 0);
    lv_obj_add_flag(about_obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(about_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(about_obj, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(about_obj, LV_SCROLLBAR_MODE_AUTO);

    int sbh = theme_status_bar_h();
    int y = sbh + SP_XS + SP_M;
    const int LINE_H = 36;
    const int LABEL_X = 20;
    const int VALUE_X_RIGHT = -20;
    // Reserve fixed widths so the right-aligned value doesn't crash through the label.
    const int LABEL_W = 110;
    const int VALUE_W = caps.width - LABEL_X - LABEL_W - 16 - 20;

    auto add_row = [&](const char* label, const char* value) {
        lv_obj_t* l = lv_label_create(about_obj);
        lv_obj_set_style_text_font(l, theme_font_body(), 0);
        lv_obj_set_style_text_color(l, THEME_DIM, 0);
        lv_label_set_text(l, label);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, LABEL_X, y);

        lv_obj_t* v = lv_label_create(about_obj);
        lv_obj_set_style_text_font(v, theme_font_body(), 0);
        lv_obj_set_style_text_color(v, THEME_TEXT, 0);
        lv_obj_set_width(v, VALUE_W);
        lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(v, value);
        lv_obj_align(v, LV_ALIGN_TOP_RIGHT, VALUE_X_RIGHT, y);
        y += LINE_H;
    };

    char ver_buf[40];
    snprintf(ver_buf, sizeof(ver_buf), "%s %s", FW_VERSION_STR, FW_VERSION);
    add_row("Version", ver_buf);
    add_row("Built",   __DATE__);
    add_row("Board",   board_caps().name);

    char macbuf[24];
    strlcpy(macbuf, ble_get_mac_address(), sizeof(macbuf));
    add_row("BLE MAC", macbuf);

    String wm = WiFi.macAddress();
    add_row("WiFi MAC", wm.c_str());

    char ubuf[32];
    format_uptime(millis() / 1000, ubuf, sizeof(ubuf));
    add_row("Uptime", ubuf);

    char hbuf[24];
    snprintf(hbuf, sizeof(hbuf), "%lu KB", (unsigned long)(ESP.getFreeHeap() / 1024));
    add_row("Free heap", hbuf);
}

static void drill_about(void) {
    build_about_view();
    current_view = MENU_VIEW_ABOUT;
    if (grid_obj)  lv_obj_add_flag(grid_obj, LV_OBJ_FLAG_HIDDEN);
    if (about_obj) lv_obj_clear_flag(about_obj, LV_OBJ_FLAG_HIDDEN);
    ui_status_bar_set_title("< ABOUT");
    ui_status_bar_set_back_cb(show_list_view);
    touch_wait_release();
}

// ----- Factory reset drill (Phase 7) -----
static void factory_reset_perform(void) {
    Serial.println("factory: WIPE — pet + clawd_cfg + settings");
    {
        Preferences p;
        if (p.begin("pet", false))         { p.clear(); p.end(); }
        if (p.begin("clawdmeter", false))  { p.clear(); p.end(); }
    }
    settings::wipe();
    delay(200);
    Serial.println("factory: reboot");
    ESP.restart();
}

static void freset_press_cb(lv_event_t* e) {
    freset_press_started_ms = lv_tick_get();
    if (freset_bar) lv_bar_set_value(freset_bar, 0, LV_ANIM_OFF);
}

static void freset_release_cb(lv_event_t* e) {
    freset_press_started_ms = 0;
    if (freset_bar) lv_bar_set_value(freset_bar, 0, LV_ANIM_ON);
}

static void build_freset_view(void) {
    if (freset_obj) return;
    const BoardCaps& caps = board_caps();

    freset_obj = lv_obj_create(root_obj);
    lv_obj_remove_style_all(freset_obj);
    lv_obj_set_size(freset_obj, caps.width, caps.height);
    lv_obj_set_style_bg_color(freset_obj, THEME_BG, 0);
    lv_obj_set_style_bg_opa(freset_obj, LV_OPA_COVER, 0);
    lv_obj_add_flag(freset_obj, LV_OBJ_FLAG_HIDDEN);

    int sbh = theme_status_bar_h();
    int y = sbh + SP_XL + SP_XL;

    lv_obj_t* title = lv_label_create(freset_obj);
    lv_label_set_text(title, "FACTORY RESET");
    lv_obj_set_style_text_font(title, theme_font_display_s(), 0);
    lv_obj_set_style_text_color(title, THEME_AMBER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, y);
    y += 60;

    lv_obj_t* warn = lv_label_create(freset_obj);
    lv_label_set_text(warn,
        "Wipes pet, gallery, WiFi, and settings.\n"
        "Cannot be undone.\n\n"
        "Hold the button for 3 seconds to confirm.");
    lv_obj_set_style_text_font(warn, theme_font_body(), 0);
    lv_obj_set_style_text_color(warn, THEME_TEXT, 0);
    lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(warn, caps.width - 40);
    lv_obj_align(warn, LV_ALIGN_TOP_MID, 0, y);

    int btn_h = 80;
    freset_btn = lv_obj_create(freset_obj);
    lv_obj_remove_style_all(freset_btn);
    lv_obj_set_size(freset_btn, caps.width - 80, btn_h);
    lv_obj_align(freset_btn, LV_ALIGN_BOTTOM_MID, 0, -(btn_h + SP_XL));
    lv_obj_set_style_bg_color(freset_btn, THEME_AMBER, 0);
    lv_obj_set_style_bg_opa(freset_btn, LV_OPA_30, 0);
    lv_obj_set_style_radius(freset_btn, 40, 0);
    lv_obj_add_flag(freset_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(freset_btn, freset_press_cb,   LV_EVENT_PRESSED,    nullptr);
    lv_obj_add_event_cb(freset_btn, freset_release_cb, LV_EVENT_RELEASED,   nullptr);
    lv_obj_add_event_cb(freset_btn, freset_release_cb, LV_EVENT_PRESS_LOST, nullptr);
    {
        lv_obj_t* l = lv_label_create(freset_btn);
        lv_label_set_text(l, "Hold to reset");
        lv_obj_set_style_text_color(l, THEME_TEXT, 0);
        lv_obj_center(l);
    }

    freset_bar = lv_bar_create(freset_obj);
    lv_obj_set_size(freset_bar, caps.width - 80, 8);
    lv_obj_align_to(freset_bar, freset_btn, LV_ALIGN_OUT_TOP_MID, 0, -SP_S);
    lv_bar_set_range(freset_bar, 0, FRESET_HOLD_MS);
    lv_obj_set_style_bg_color(freset_bar, THEME_PANEL, 0);
    lv_obj_set_style_bg_color(freset_bar, THEME_AMBER, LV_PART_INDICATOR);
}

static const char* freset_secondary(void) { return ""; }

static void drill_freset(void) {
    build_freset_view();
    freset_press_started_ms = 0;
    if (freset_bar) lv_bar_set_value(freset_bar, 0, LV_ANIM_OFF);
    current_view = MENU_VIEW_FRESET;
    if (grid_obj)   lv_obj_add_flag(grid_obj, LV_OBJ_FLAG_HIDDEN);
    if (freset_obj) lv_obj_clear_flag(freset_obj, LV_OBJ_FLAG_HIDDEN);
    ui_status_bar_set_title("< RESET");
    ui_status_bar_set_back_cb(show_list_view);
    touch_wait_release();
}

// ----- WiFi info drill (Phase 6) -----
static void wifi_refresh_labels(void) {
    if (!wifi_obj) return;
    char buf[64];

    String ssid = WiFi.SSID();
    lv_label_set_text(wifi_lbl_ssid, ssid.length() ? ssid.c_str() : "(not connected)");

    snprintf(buf, sizeof(buf), "%d dBm", (int)WiFi.RSSI());
    lv_label_set_text(wifi_lbl_rssi, buf);

    IPAddress ip = WiFi.localIP();
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    lv_label_set_text(wifi_lbl_ip, buf);

    String url, token;
    cfg::load_daemon(url, token);
    if (!url.length()) {
        lv_label_set_text(wifi_lbl_url, "(none)");
    } else {
        // Strip the scheme — the value column is narrow and "https://" steals
        // 8 chars every host loses to the ellipsis.
        const char* p = url.c_str();
        if      (strncmp(p, "https://", 8) == 0) p += 8;
        else if (strncmp(p, "http://",  7) == 0) p += 7;
        lv_label_set_text(wifi_lbl_url, p);
    }

    uint32_t last = net_daemon::last_apply_ts();
    if (last == 0) snprintf(buf, sizeof(buf), "never");
    else snprintf(buf, sizeof(buf), "%lus ago", (unsigned long)((millis() - last) / 1000));
    lv_label_set_text(wifi_lbl_poll, buf);

    // Connection-status line: green = connected + daemon data fresh, amber =
    // connected but no fresh daemon data, grey = not on a network.
    if (wifi_lbl_status && wifi_status_dot) {
        lv_color_t col; const char* txt;
        if (!ssid.length())                 { col = THEME_DIM;   txt = "Disconnected"; }
        else if (net_daemon::state_fresh())  { col = THEME_GREEN; txt = "Connected"; }
        else                                 { col = THEME_AMBER; txt = "Connected - no data"; }
        lv_obj_set_style_bg_color(wifi_status_dot, col, 0);
        lv_label_set_text(wifi_lbl_status, txt);
        lv_obj_set_style_text_color(wifi_lbl_status, col, 0);
    }
}

static void wifi_action_reconnect_cb(lv_event_t* e) {
    wifi_action_state      = WAS_RECONNECTING;
    wifi_action_started_ms = lv_tick_get();
    lv_label_set_text(wifi_lbl_action, "Reconnecting...");
    lv_obj_set_style_text_color(wifi_lbl_action, THEME_TEXT, 0);
    WiFi.reconnect();
}

static void wifi_action_switch_cb(lv_event_t* e) {
    cfg::clear_wifi_only();
    Serial.println("wifi: switch network — clearing creds and rebooting into provisioning");
    delay(200);
    ESP.restart();
}

static void wifi_apply_visibility(void) {
    bool on = settings::radio_wifi_enabled();
    if (wifi_content) (on ? lv_obj_clear_flag : lv_obj_add_flag)(wifi_content, LV_OBJ_FLAG_HIDDEN);
    if (wifi_offmsg)  (on ? lv_obj_add_flag  : lv_obj_clear_flag)(wifi_offmsg, LV_OBJ_FLAG_HIDDEN);
}

static void wifi_switch_cb(lv_event_t* e) {
    bool on = lv_obj_has_state((lv_obj_t*)lv_event_get_target(e), LV_STATE_CHECKED);
    radio::set_wifi(on);                 // forces BLE off when turning WiFi on
    wifi_apply_visibility();
    menu_screen_refresh_secondary_labels();   // update the grid tile status line
}

static void build_wifi_view(void) {
    if (wifi_obj) return;
    const BoardCaps& caps = board_caps();
    int sbh = theme_status_bar_h();

    wifi_obj = lv_obj_create(root_obj);
    lv_obj_remove_style_all(wifi_obj);
    lv_obj_set_size(wifi_obj, caps.width, caps.height);
    lv_obj_set_style_bg_color(wifi_obj, THEME_BG, 0);
    lv_obj_set_style_bg_opa(wifi_obj, LV_OPA_COVER, 0);
    lv_obj_add_flag(wifi_obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(wifi_obj, LV_OBJ_FLAG_SCROLLABLE);

    // Switch row
    lv_obj_t* swrow = lv_obj_create(wifi_obj);
    lv_obj_remove_style_all(swrow);
    lv_obj_set_size(swrow, caps.width - 40, 58);
    lv_obj_set_pos(swrow, 20, sbh + SP_S);
    lv_obj_set_style_bg_color(swrow, THEME_PANEL, 0);
    lv_obj_set_style_bg_opa(swrow, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(swrow, 12, 0);
    lv_obj_clear_flag(swrow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* swlbl = lv_label_create(swrow);
    lv_label_set_text(swlbl, "WiFi");
    lv_obj_set_style_text_font(swlbl, theme_font_display_s(), 0);
    lv_obj_set_style_text_color(swlbl, THEME_TEXT, 0);
    lv_obj_align(swlbl, LV_ALIGN_LEFT_MID, 16, 0);
    wifi_sw = lv_switch_create(swrow);
    lv_obj_set_size(wifi_sw, 64, 34);
    lv_obj_align(wifi_sw, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_bg_color(wifi_sw, THEME_BAR_BG, 0);
    lv_obj_set_style_bg_color(wifi_sw, THEME_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(wifi_sw, THEME_TEXT, LV_PART_KNOB);
    lv_obj_add_event_cb(wifi_sw, wifi_switch_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    // Single-radio note
    wifi_note = lv_label_create(wifi_obj);
    lv_label_set_text(wifi_note, "Turning WiFi on switches Bluetooth off.");
    lv_obj_set_style_text_font(wifi_note, theme_font_label(), 0);
    lv_obj_set_style_text_color(wifi_note, THEME_DIM, 0);
    lv_obj_set_pos(wifi_note, 22, sbh + SP_S + 58 + SP_XS);

    // Content container (info rows + actions) — toggled as a unit in the off-state
    int content_y = sbh + SP_S + 58 + SP_XS + 26 + SP_S;
    wifi_content = lv_obj_create(wifi_obj);
    lv_obj_remove_style_all(wifi_content);
    lv_obj_set_size(wifi_content, caps.width, caps.height - content_y);
    lv_obj_set_pos(wifi_content, 0, content_y);
    lv_obj_set_style_bg_opa(wifi_content, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(wifi_content, LV_OBJ_FLAG_SCROLLABLE);

    // Grouped info card (matches the switch row + tile aesthetic). Holds a
    // colored connection-status line then the label/value rows.
    const int card_x = 20;
    const int card_w = caps.width - 40;
    const int LINE_H = 40, LABEL_W = 110;
    const int rows_top = 44;                 // below the status line
    const int card_h = rows_top + 5 * LINE_H + SP_M;

    wifi_card = lv_obj_create(wifi_content);
    lv_obj_remove_style_all(wifi_card);
    lv_obj_set_size(wifi_card, card_w, card_h);
    lv_obj_set_pos(wifi_card, card_x, 0);
    lv_obj_set_style_bg_color(wifi_card, THEME_PANEL, 0);
    lv_obj_set_style_bg_opa(wifi_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(wifi_card, 12, 0);
    lv_obj_clear_flag(wifi_card, LV_OBJ_FLAG_SCROLLABLE);

    // Connection-status line: colored dot + text (set in wifi_refresh_labels).
    wifi_status_dot = lv_obj_create(wifi_card);
    lv_obj_remove_style_all(wifi_status_dot);
    lv_obj_set_size(wifi_status_dot, 12, 12);
    lv_obj_set_style_radius(wifi_status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(wifi_status_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(wifi_status_dot, THEME_DIM, 0);
    lv_obj_align(wifi_status_dot, LV_ALIGN_TOP_LEFT, SP_M, 18);
    wifi_lbl_status = lv_label_create(wifi_card);
    lv_obj_set_style_text_font(wifi_lbl_status, theme_font_body(), 0);
    lv_obj_set_style_text_color(wifi_lbl_status, THEME_TEXT, 0);
    lv_label_set_text(wifi_lbl_status, "Disconnected");
    lv_obj_align(wifi_lbl_status, LV_ALIGN_TOP_LEFT, SP_M + 12 + SP_S, 12);

    int y = rows_top;
    int VALUE_W = card_w - SP_M - LABEL_W - SP_M;
    auto add_row = [&](lv_obj_t** out, const char* label) {
        lv_obj_t* l = lv_label_create(wifi_card);
        lv_obj_set_style_text_font(l, theme_font_body(), 0);
        lv_obj_set_style_text_color(l, THEME_DIM, 0);
        lv_label_set_text(l, label);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, SP_M, y + 4);
        lv_obj_t* v = lv_label_create(wifi_card);
        lv_obj_set_style_text_font(v, theme_font_body(), 0);
        lv_obj_set_style_text_color(v, THEME_TEXT, 0);
        lv_obj_set_width(v, VALUE_W);
        lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -SP_M, y + 4);
        *out = v; y += LINE_H;
    };
    add_row(&wifi_lbl_ssid, "Network");
    add_row(&wifi_lbl_rssi, "Signal");
    add_row(&wifi_lbl_ip,   "IP");
    add_row(&wifi_lbl_url,  "Daemon");
    add_row(&wifi_lbl_poll, "Last poll");

    int btn_w = (caps.width - 60) / 2;
    int btn_h = 60;
    int by = card_h + SP_M;

    lv_obj_t* recon = lv_obj_create(wifi_content);
    lv_obj_remove_style_all(recon);
    lv_obj_set_size(recon, btn_w, btn_h);
    lv_obj_set_style_bg_color(recon, THEME_PANEL, 0);
    lv_obj_set_style_bg_opa(recon, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(recon, 12, 0);
    lv_obj_align(recon, LV_ALIGN_TOP_LEFT, 20, by);
    lv_obj_add_flag(recon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(recon, wifi_action_reconnect_cb, LV_EVENT_CLICKED, nullptr);
    { lv_obj_t* l = lv_label_create(recon); lv_label_set_text(l, "Reconnect");
      lv_obj_set_style_text_color(l, THEME_TEXT, 0); lv_obj_center(l); }

    lv_obj_t* swbtn = lv_obj_create(wifi_content);
    lv_obj_remove_style_all(swbtn);
    lv_obj_set_size(swbtn, btn_w, btn_h);
    lv_obj_set_style_bg_color(swbtn, THEME_ACCENT, 0);
    lv_obj_set_style_bg_opa(swbtn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(swbtn, 12, 0);
    lv_obj_align(swbtn, LV_ALIGN_TOP_RIGHT, -20, by);
    lv_obj_add_flag(swbtn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(swbtn, wifi_action_switch_cb, LV_EVENT_CLICKED, nullptr);
    { lv_obj_t* l = lv_label_create(swbtn); lv_label_set_text(l, "Switch");
      lv_obj_set_style_text_color(l, THEME_BG, 0); lv_obj_center(l); }

    // Transient reconnect status. Sits BELOW the action buttons (label font) so
    // it never overlaps the info rows, which now fill down to the buttons on
    // both the 480- and 448-tall boards.
    wifi_lbl_action = lv_label_create(wifi_content);
    lv_obj_set_style_text_font(wifi_lbl_action, theme_font_label(), 0);
    lv_obj_align(wifi_lbl_action, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_label_set_text(wifi_lbl_action, "");

    // Off-state message (sibling of content)
    wifi_offmsg = lv_label_create(wifi_obj);
    lv_label_set_text(wifi_offmsg,
        "WiFi is off.\nSession & usage won't update\nuntil you turn it back on.");
    lv_obj_set_style_text_font(wifi_offmsg, theme_font_body(), 0);
    lv_obj_set_style_text_color(wifi_offmsg, THEME_DIM, 0);
    lv_obj_set_style_text_align(wifi_offmsg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(wifi_offmsg, LV_ALIGN_CENTER, 0, sbh / 2 + 20);
    lv_obj_add_flag(wifi_offmsg, LV_OBJ_FLAG_HIDDEN);
}

static const char* wifi_secondary(void) {
    return WiFi.SSID().length() ? "Connected" : "Off";
}

static void drill_wifi(void) {
    build_wifi_view();
    wifi_refresh_labels();
    if (wifi_sw) {
        if (settings::radio_wifi_enabled()) lv_obj_add_state(wifi_sw, LV_STATE_CHECKED);
        else                                lv_obj_remove_state(wifi_sw, LV_STATE_CHECKED);
    }
    wifi_apply_visibility();
    current_view = MENU_VIEW_WIFI;
    if (grid_obj) lv_obj_add_flag(grid_obj, LV_OBJ_FLAG_HIDDEN);
    if (wifi_obj) lv_obj_clear_flag(wifi_obj, LV_OBJ_FLAG_HIDDEN);
    ui_status_bar_set_title("< WIFI");
    ui_status_bar_set_back_cb(show_list_view);
    touch_wait_release();
}

// ----- Splash picker drill (carousel) -----
static void sp_free_buf(void) {
    if (sp_canvas_buf) {
        heap_caps_free(sp_canvas_buf);
        sp_canvas_buf = nullptr;
    }
}

// Map sp_browse_idx → species id to render (0 means "Auto" which rotates
// through species on its own SP_AUTO_ROTATE_MS schedule).
static uint16_t sp_species_for_browse(void) {
    if (sp_browse_idx == 0) return (uint16_t)sp_auto_species;
    return (uint16_t)(sp_browse_idx - 1);
}

// Render whatever species the carousel is currently parked on, at the
// current frame. Cheap — LVGL marks the canvas dirty for us.
static void sp_render_current(void) {
    if (!sp_canvas_buf) return;
    splash_render_to_buf(sp_canvas_buf, sp_canvas_px, sp_cell,
                         sp_species_for_browse(), sp_frame_idx);
    if (sp_canvas) lv_obj_invalidate(sp_canvas);
}

// Sync the name + counter labels + the Set-button label/state to whatever
// browse position we're parked on.
static void sp_refresh_labels(void) {
    if (!sp_name_lbl || !sp_counter_lbl || !sp_set_btn_lbl) return;
    char buf[24];
    if (sp_browse_idx == 0) {
        lv_label_set_text(sp_name_lbl, "Auto");
    } else {
        lv_label_set_text(sp_name_lbl, pet_species_name((uint16_t)(sp_browse_idx - 1)));
    }
    snprintf(buf, sizeof(buf), "%d of 14", sp_browse_idx + 1);
    lv_label_set_text(sp_counter_lbl, buf);

    int8_t pin = settings::splash_pin();
    bool is_current = (pin == -1 && sp_browse_idx == 0)
                   || (pin >= 0 && sp_browse_idx == pin + 1);
    if (is_current) {
        lv_label_set_text(sp_set_btn_lbl, "Current splash");
        lv_obj_set_style_bg_color(sp_set_btn, THEME_PANEL, 0);
        lv_obj_set_style_text_color(sp_set_btn_lbl, THEME_DIM, 0);
    } else {
        lv_label_set_text(sp_set_btn_lbl, "Set as splash");
        lv_obj_set_style_bg_color(sp_set_btn, THEME_AMBER, 0);
        lv_obj_set_style_text_color(sp_set_btn_lbl, THEME_BG, 0);
    }
}

static void sp_step(int delta) {
    sp_browse_idx = (sp_browse_idx + delta + 14) % 14;
    sp_frame_idx  = 0;
    // If we're stepping ONTO the Auto slot, reset the auto-rotate clock so
    // the first species the user sees is the start of the rotation, not
    // wherever the background rotation timer happened to be.
    if (sp_browse_idx == 0) {
        sp_auto_rotate_ms = lv_tick_get();
        sp_auto_species   = 0;
    }
    sp_refresh_labels();
    sp_render_current();
}

static void sp_left_cb(lv_event_t* e)  { (void)e; sp_step(-1); }
static void sp_right_cb(lv_event_t* e) { (void)e; sp_step(+1); }

static void sp_set_cb(lv_event_t* e) {
    (void)e;
    if (sp_browse_idx == 0) settings::set_splash_pin(SETTINGS_SPLASH_PIN_AUTO);
    else                    settings::set_splash_pin((int8_t)(sp_browse_idx - 1));
    sp_refresh_labels();
}

static void build_splash_pick_view(void) {
    if (splash_pick_obj) return;
    const BoardCaps& caps = board_caps();
    bool large = (caps.height >= 460);

    splash_pick_obj = lv_obj_create(root_obj);
    lv_obj_remove_style_all(splash_pick_obj);
    lv_obj_set_size(splash_pick_obj, caps.width, caps.height);
    lv_obj_set_style_bg_color(splash_pick_obj, THEME_BG, 0);
    lv_obj_set_style_bg_opa(splash_pick_obj, LV_OPA_COVER, 0);
    lv_obj_add_flag(splash_pick_obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(splash_pick_obj, LV_OBJ_FLAG_SCROLLABLE);

    int sbh = theme_status_bar_h();

    // Canvas — biggest cell that fits comfortably with room for name +
    // counter + button stack underneath. 20 cells per axis.
    sp_cell      = large ? 10 : 8;
    sp_canvas_px = 20 * sp_cell;

    sp_canvas_buf = (uint16_t*)heap_caps_malloc(sp_canvas_px * sp_canvas_px * 2, MALLOC_CAP_SPIRAM);
    if (!sp_canvas_buf) {
        Serial.println("splash_pick: PSRAM canvas alloc failed");
        return;
    }
    // Initial render so the canvas isn't garbage on first paint.
    sp_browse_idx = 0;
    sp_frame_idx  = 0;
    int8_t pin = settings::splash_pin();
    if (pin >= 0) sp_browse_idx = pin + 1;
    sp_auto_species   = 0;
    sp_auto_rotate_ms = lv_tick_get();
    splash_render_to_buf(sp_canvas_buf, sp_canvas_px, sp_cell,
                         sp_species_for_browse(), 0);

    int canvas_y = sbh + SP_M + SP_S;

    sp_canvas = lv_canvas_create(splash_pick_obj);
    lv_canvas_set_buffer(sp_canvas, sp_canvas_buf, sp_canvas_px, sp_canvas_px, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(sp_canvas, LV_ALIGN_TOP_MID, 0, canvas_y);
    lv_obj_clear_flag(sp_canvas, LV_OBJ_FLAG_CLICKABLE);

    // Left/right tap zones — large invisible columns flanking the canvas.
    // Stretch from the top of the canvas to the bottom so the finger lands
    // anywhere in the gutter and still flips. Transparent bg, no border.
    int zone_w = (caps.width - sp_canvas_px) / 2 - 2;
    int zone_h = sp_canvas_px + 40;
    int zone_y = canvas_y - 20;

    auto make_zone = [&](int x, lv_obj_t** out, lv_event_cb_t cb) {
        lv_obj_t* z = lv_obj_create(splash_pick_obj);
        lv_obj_remove_style_all(z);
        lv_obj_set_size(z, zone_w, zone_h);
        lv_obj_set_pos(z, x, zone_y);
        lv_obj_set_style_bg_opa(z, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(z, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(z, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(z, cb, LV_EVENT_SHORT_CLICKED, nullptr);
        *out = z;
    };
    make_zone(0,                              &sp_left_zone,  sp_left_cb);
    make_zone(caps.width - zone_w,            &sp_right_zone, sp_right_cb);

    // Visible arrow hints (centered in each zone). Dim so the eye reads
    // them as affordances, not buttons in their own right — the whole zone
    // is the hit target.
    auto place_arrow = [&](lv_obj_t* parent, const char* glyph) {
        lv_obj_t* l = lv_label_create(parent);
        lv_label_set_text(l, glyph);
        lv_obj_set_style_text_font(l, theme_font_display_s(), 0);
        lv_obj_set_style_text_color(l, THEME_DIM, 0);
        lv_obj_center(l);
    };
    place_arrow(sp_left_zone,  "<");
    place_arrow(sp_right_zone, ">");

    // Name (display_s) + counter (label, dim) stacked under the canvas.
    int below_y = canvas_y + sp_canvas_px + (large ? SP_L : SP_S);

    sp_name_lbl = lv_label_create(splash_pick_obj);
    lv_obj_set_style_text_font(sp_name_lbl, theme_font_display_s(), 0);
    lv_obj_set_style_text_color(sp_name_lbl, THEME_TEXT, 0);
    lv_label_set_text(sp_name_lbl, "");
    lv_obj_align(sp_name_lbl, LV_ALIGN_TOP_MID, 0, below_y);

    sp_counter_lbl = lv_label_create(splash_pick_obj);
    lv_obj_set_style_text_font(sp_counter_lbl, theme_font_body(), 0);
    lv_obj_set_style_text_color(sp_counter_lbl, THEME_DIM, 0);
    lv_label_set_text(sp_counter_lbl, "");
    lv_obj_align(sp_counter_lbl, LV_ALIGN_TOP_MID, 0,
                 below_y + (large ? 44 : 32));

    // Set-as-splash button anchored to the bottom.
    int btn_h = large ? 64 : 52;
    sp_set_btn = lv_obj_create(splash_pick_obj);
    lv_obj_remove_style_all(sp_set_btn);
    lv_obj_set_size(sp_set_btn, caps.width - 80, btn_h);
    lv_obj_align(sp_set_btn, LV_ALIGN_BOTTOM_MID, 0, -SP_L);
    lv_obj_set_style_bg_color(sp_set_btn, THEME_AMBER, 0);
    lv_obj_set_style_bg_opa(sp_set_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sp_set_btn, btn_h / 2, 0);
    lv_obj_clear_flag(sp_set_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sp_set_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sp_set_btn, sp_set_cb, LV_EVENT_SHORT_CLICKED, nullptr);

    sp_set_btn_lbl = lv_label_create(sp_set_btn);
    lv_obj_set_style_text_font(sp_set_btn_lbl, theme_font_body(), 0);
    lv_obj_set_style_text_color(sp_set_btn_lbl, THEME_BG, 0);
    lv_label_set_text(sp_set_btn_lbl, "");
    lv_obj_center(sp_set_btn_lbl);

    sp_refresh_labels();
}

static const char* splash_pick_secondary(void) {
    int8_t pin = settings::splash_pin();
    return (pin < 0) ? "Auto" : pet_species_name((uint16_t)pin);
}

static void drill_splash_pick(void) {
    build_splash_pick_view();
    sp_refresh_labels();
    sp_render_current();
    current_view = MENU_VIEW_SPLASHPICK;
    if (grid_obj)        lv_obj_add_flag(grid_obj, LV_OBJ_FLAG_HIDDEN);
    if (splash_pick_obj) lv_obj_clear_flag(splash_pick_obj, LV_OBJ_FLAG_HIDDEN);
    ui_status_bar_set_title("< SPLASH");
    ui_status_bar_set_back_cb(show_list_view);
    touch_wait_release();
}

// ----- Pet info drill (Phase 4) -----
static const char* stage_name(uint8_t stage) {
    static const char* NAMES[5] = { "egg", "hatchling", "juvenile", "adult", "elder" };
    return (stage < 5) ? NAMES[stage] : "?";
}

// Builds a fact tile: small dim label above a scaled-up value, the pair
// flex-centered so it stays balanced when the tile is tall. Returns the
// value label so the caller can update it.
static lv_obj_t* petinfo_make_tile(int x, int y, int w, int h,
                                   const char* label) {
    lv_obj_t* card = lv_obj_create(petinfo_obj);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, THEME_PANEL, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 12, 0);   // match menu/achievements tile radius
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(card, SP_S, 0);
    lv_obj_set_style_pad_row(card, SP_XS, 0);

    lv_obj_t* lbl = lv_label_create(card);
    lv_obj_set_style_text_font(lbl, theme_font_label(), 0);
    lv_obj_set_style_text_color(lbl, THEME_DIM, 0);
    lv_label_set_text(lbl, label);

    lv_obj_t* val = lv_label_create(card);
    lv_obj_set_style_text_font(val, theme_font_display_s(), 0);
    lv_obj_set_style_text_color(val, THEME_TEXT, 0);
    lv_label_set_text(val, "");
    return val;
}

static void petinfo_ensure_widgets(void) {
    if (petinfo_hero_name) return;
    const BoardCaps& caps = board_caps();
    int sbh = theme_status_bar_h();
    int margin = SP_L;
    int gap    = SP_M;
    int w = caps.width - 2 * margin;

    // Fill the whole screen below the status bar: hero + 3 tile rows + 3 gaps
    // distributed across the available height (was top-loaded, leaving the
    // bottom third empty).
    int y0     = sbh + SP_M;
    int bottom = caps.height - SP_L;
    int hero_h = PETINFO_SPRITE_PX + 2 * SP_M;            // 80 + 24 = 104
    int row_h  = (bottom - y0 - hero_h - 3 * gap) / 3;
    int col_w  = (w - gap) / 2;
    int y = y0;

    // ---- Hero card: sprite + name + level/stage ----
    lv_obj_t* hero = lv_obj_create(petinfo_obj);
    lv_obj_remove_style_all(hero);
    lv_obj_set_pos(hero, margin, y);
    lv_obj_set_size(hero, w, hero_h);
    lv_obj_set_style_bg_color(hero, THEME_PANEL, 0);
    lv_obj_set_style_bg_opa(hero, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(hero, 12, 0);   // match menu/achievements tile radius
    lv_obj_clear_flag(hero, LV_OBJ_FLAG_SCROLLABLE);

    petinfo_sprite_buf = (uint16_t*)heap_caps_malloc(
        PETINFO_SPRITE_PX * PETINFO_SPRITE_PX * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (petinfo_sprite_buf) {
        petinfo_hero_canvas = lv_canvas_create(hero);
        lv_canvas_set_buffer(petinfo_hero_canvas, petinfo_sprite_buf,
                             PETINFO_SPRITE_PX, PETINFO_SPRITE_PX, LV_COLOR_FORMAT_RGB565);
        lv_obj_align(petinfo_hero_canvas, LV_ALIGN_LEFT_MID, SP_M, 0);
    }

    petinfo_hero_name = lv_label_create(hero);
    lv_obj_set_style_text_font(petinfo_hero_name, theme_font_display_s(), 0);
    lv_obj_set_style_text_color(petinfo_hero_name, THEME_TEXT, 0);
    lv_label_set_text(petinfo_hero_name, "");
    lv_obj_align(petinfo_hero_name, LV_ALIGN_LEFT_MID, SP_M + PETINFO_SPRITE_PX + SP_M, -16);

    petinfo_hero_sub = lv_label_create(hero);
    lv_obj_set_style_text_font(petinfo_hero_sub, theme_font_body(), 0);
    lv_obj_set_style_text_color(petinfo_hero_sub, THEME_DIM, 0);
    lv_label_set_text(petinfo_hero_sub, "");
    lv_obj_align(petinfo_hero_sub, LV_ALIGN_LEFT_MID, SP_M + PETINFO_SPRITE_PX + SP_M, 18);

    y += hero_h + gap;

    // ---- Fact tiles: 2 columns + 1 full-width, sized to fill the rest ----
    petinfo_val_species = petinfo_make_tile(margin,                y, col_w, row_h, "SPECIES");
    petinfo_val_born    = petinfo_make_tile(margin + col_w + gap,  y, col_w, row_h, "BORN");
    y += row_h + gap;
    petinfo_val_total   = petinfo_make_tile(margin,                y, col_w, row_h, "TOTAL XP");
    petinfo_val_tonext  = petinfo_make_tile(margin + col_w + gap,  y, col_w, row_h, "TO NEXT");
    y += row_h + gap;
    petinfo_val_evolve  = petinfo_make_tile(margin,                y, w,     row_h, "NEXT EVOLVE");
}

static void petinfo_refresh_values(void) {
    const Pet* p = pet_current();
    uint8_t stage = pet_evolution_stage(p);
    char buf[40];

    // Hero sprite (frame 0) + name + level/stage.
    if (petinfo_hero_canvas && petinfo_sprite_buf) {
        splash_render_to_buf(petinfo_sprite_buf, PETINFO_SPRITE_PX, PETINFO_SPRITE_CELL,
                             pet_species_animation_index(p->species_id), 0);
        lv_obj_invalidate(petinfo_hero_canvas);
    }
    lv_label_set_text(petinfo_hero_name, p->name);   // pet name lives on the Pet struct
    snprintf(buf, sizeof(buf), "Lv %u - %s", (unsigned)p->level, stage_name(stage));
    lv_label_set_text(petinfo_hero_sub, buf);

    lv_label_set_text(petinfo_val_species, pet_species_name(p->species_id));

    // pet_now() isn't public, so use a rough "uptime since process started"
    // approximation for "Born X days ago" (same as the prior pet-info view).
    uint32_t age_sec = millis() / 1000;
    uint32_t days = age_sec / 86400;
    if (days > 0) snprintf(buf, sizeof(buf), "%lud ago", (unsigned long)days);
    else          snprintf(buf, sizeof(buf), "today");
    lv_label_set_text(petinfo_val_born, buf);

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)pet_total_xp_earned());
    lv_label_set_text(petinfo_val_total, buf);

    if (p->level >= PET_MAX_LEVEL) {
        lv_label_set_text(petinfo_val_tonext, "MAX");
        lv_label_set_text(petinfo_val_evolve, "MAX");
    } else {
        uint32_t cost = pet_xp_to_next(p->level);
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)(cost - p->xp));
        lv_label_set_text(petinfo_val_tonext, buf);
        uint8_t next_evo = (p->level < 4) ? 4 : (p->level < 9) ? 9 : (p->level < 17) ? 17 : 30;
        snprintf(buf, sizeof(buf), "Lv %u", (unsigned)next_evo);
        lv_label_set_text(petinfo_val_evolve, buf);
    }
}

static void build_petinfo_view(void) {
    if (petinfo_obj) return;
    const BoardCaps& caps = board_caps();

    petinfo_obj = lv_obj_create(root_obj);
    lv_obj_remove_style_all(petinfo_obj);
    lv_obj_set_size(petinfo_obj, caps.width, caps.height);
    lv_obj_set_style_bg_color(petinfo_obj, THEME_BG, 0);
    lv_obj_set_style_bg_opa(petinfo_obj, LV_OPA_COVER, 0);
    lv_obj_add_flag(petinfo_obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(petinfo_obj, LV_OBJ_FLAG_SCROLLABLE);   // bio layout fits 480h
}

static const char* petinfo_secondary(void) {
    static char buf[16];
    const Pet* p = pet_current();
    snprintf(buf, sizeof(buf), "Lv %u", (unsigned)p->level);
    return buf;
}

static void drill_petinfo(void) {
    build_petinfo_view();
    petinfo_ensure_widgets();
    petinfo_refresh_values();
    current_view = MENU_VIEW_PETINFO;
    if (grid_obj)    lv_obj_add_flag(grid_obj, LV_OBJ_FLAG_HIDDEN);
    if (petinfo_obj) lv_obj_clear_flag(petinfo_obj, LV_OBJ_FLAG_HIDDEN);
    ui_status_bar_set_title("< PET INFO");
    ui_status_bar_set_back_cb(show_list_view);
    touch_wait_release();
}


// ----- Brightness drill (Phase 3) -----
static const uint8_t BRIGHT_LEVELS[3] = {
    SETTINGS_BRIGHTNESS_LOW, SETTINGS_BRIGHTNESS_MED, SETTINGS_BRIGHTNESS_HIGH
};
static const char* BRIGHT_LABELS[3] = { "Low", "Medium", "High" };

static void brightness_refresh_selection(void) {
    uint8_t cur = settings::brightness();
    int sel = 1;
    for (int i = 0; i < 3; i++) if (BRIGHT_LEVELS[i] == cur) sel = i;
    for (int i = 0; i < 3; i++) {
        if (!brightness_pills[i]) continue;
        lv_obj_set_style_bg_color(brightness_pills[i],
            i == sel ? THEME_AMBER : THEME_PANEL, 0);
        lv_obj_t* lbl = lv_obj_get_child(brightness_pills[i], 0);
        if (lbl) lv_obj_set_style_text_color(lbl, i == sel ? THEME_BG : THEME_TEXT, 0);
    }
}

static void brightness_pill_cb(lv_event_t* e) {
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx > 2) return;
    uint8_t v = BRIGHT_LEVELS[idx];
    display_hal_set_brightness(v);
    settings::set_brightness(v);
    brightness_refresh_selection();
}

static void build_brightness_view(void) {
    if (brightness_obj) return;
    const BoardCaps& caps = board_caps();

    brightness_obj = lv_obj_create(root_obj);
    lv_obj_remove_style_all(brightness_obj);
    lv_obj_set_size(brightness_obj, caps.width, caps.height);
    lv_obj_set_style_bg_color(brightness_obj, THEME_BG, 0);
    lv_obj_set_style_bg_opa(brightness_obj, LV_OPA_COVER, 0);
    lv_obj_add_flag(brightness_obj, LV_OBJ_FLAG_HIDDEN);

    int sbh = theme_status_bar_h();
    int y = sbh + SP_XS + SP_M + SP_XL;
    int pill_h = 80;
    int pill_gap = 16;

    for (int i = 0; i < 3; i++) {
        lv_obj_t* b = lv_obj_create(brightness_obj);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, caps.width - 80, pill_h);
        lv_obj_align(b, LV_ALIGN_TOP_MID, 0, y + i * (pill_h + pill_gap));
        lv_obj_set_style_bg_color(b, THEME_PANEL, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, 40, 0);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, brightness_pill_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        lv_obj_t* lbl = lv_label_create(b);
        lv_label_set_text(lbl, BRIGHT_LABELS[i]);
        lv_obj_set_style_text_font(lbl, theme_font_display_s(), 0);
        lv_obj_set_style_text_color(lbl, THEME_TEXT, 0);
        lv_obj_center(lbl);

        brightness_pills[i] = b;
    }
    brightness_refresh_selection();
}

static const char* brightness_secondary(void) {
    uint8_t v = settings::brightness();
    if      (v == SETTINGS_BRIGHTNESS_LOW)  return "Low";
    else if (v == SETTINGS_BRIGHTNESS_MED)  return "Med";
    else                                    return "High";
}

static void drill_brightness(void) {
    build_brightness_view();
    current_view = MENU_VIEW_BRIGHTNESS;
    if (grid_obj)       lv_obj_add_flag(grid_obj, LV_OBJ_FLAG_HIDDEN);
    if (brightness_obj) lv_obj_clear_flag(brightness_obj, LV_OBJ_FLAG_HIDDEN);
    brightness_refresh_selection();
    ui_status_bar_set_title("< BRIGHTNESS");
    ui_status_bar_set_back_cb(show_list_view);
    touch_wait_release();
}

// ----- Refresh now action + toast (Phase 2) -----
enum refresh_toast_state_t { RT_IDLE, RT_REQUESTED, RT_OK, RT_FAIL };
static refresh_toast_state_t refresh_toast_state = RT_IDLE;
static uint32_t              refresh_toast_started_ms = 0;
static uint32_t              refresh_toast_baseline_apply = 0;
static lv_obj_t*             refresh_toast_lbl = nullptr;
#define REFRESH_TIMEOUT_MS    10000
#define REFRESH_AUTOCLEAR_MS  2000

static void refresh_toast_ensure(void) {
    if (refresh_toast_lbl || !root_obj) return;
    // Parent to root_obj (the menu container), NOT grid_obj. Parenting to
    // grid_obj would put the toast inside the grid container which clips it.
    // root_obj has no layout, so align actually works.
    refresh_toast_lbl = lv_label_create(root_obj);
    lv_obj_set_style_bg_color(refresh_toast_lbl, THEME_PANEL, 0);
    lv_obj_set_style_bg_opa(refresh_toast_lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(refresh_toast_lbl, 8, 0);
    lv_obj_set_style_pad_all(refresh_toast_lbl, SP_M, 0);
    lv_obj_set_style_text_font(refresh_toast_lbl, theme_font_body(), 0);
    lv_obj_set_style_text_color(refresh_toast_lbl, THEME_TEXT, 0);
    lv_obj_align(refresh_toast_lbl, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_add_flag(refresh_toast_lbl, LV_OBJ_FLAG_HIDDEN);
}

static void refresh_toast_show(const char* text, lv_color_t color) {
    refresh_toast_ensure();
    if (!refresh_toast_lbl) return;
    lv_label_set_text(refresh_toast_lbl, text);
    lv_obj_set_style_text_color(refresh_toast_lbl, color, 0);
    lv_obj_clear_flag(refresh_toast_lbl, LV_OBJ_FLAG_HIDDEN);
}

static void refresh_toast_hide(void) {
    if (refresh_toast_lbl) lv_obj_add_flag(refresh_toast_lbl, LV_OBJ_FLAG_HIDDEN);
}

static const char* refresh_secondary(void) { return ""; }

static void drill_refresh(void) {
    refresh_toast_baseline_apply = app_last_apply_ms();
    refresh_toast_started_ms     = lv_tick_get();
    refresh_toast_state          = RT_REQUESTED;
    refresh_toast_show("Refreshing...", THEME_TEXT);
    if (net_daemon::state_fresh()) net_daemon::request_refresh();
    else                           ble_request_refresh();
}

// ---- Rename Pet (Task 1.3) ----
// Opens the keyboard overlay pre-filled with the pet's current name.
// on_done writes straight through to pet_rename(), which persists via NVS.
static void on_rename_done(const char* text, void* user_data) {
    (void)user_data;
    if (!text || !text[0]) return;
    pet_rename(text);
}

static const char* rename_secondary(void) {
    const Pet* p = pet_current();
    return (p && p->name[0]) ? p->name : "";
}

static void drill_rename(void) {
    const Pet* p = pet_current();
    kbd_overlay_show("Rename pet",
                     (p && p->name[0]) ? p->name : "",
                     15,
                     on_rename_done,
                     nullptr);
    // No view-state change — keyboard overlays on top of the current menu view.
    // Touch-wait-release so the tap that opened the drill doesn't
    // immediately land on a keyboard key.
    touch_wait_release();
}

// ---- Retirement Home (Task 4.1) ----
static const char* retirement_secondary(void) {
    static char buf[8];
    const Gallery* g = pet_gallery();
    snprintf(buf, sizeof(buf), "%u", g ? (unsigned)g->count : 0);
    return buf;
}

static void retirement_open_cb(void) {
    ui_show_screen(SCREEN_RETIREMENT);
}

// ---- Stub callbacks for reserved future items ----
static const char* coming_soon_secondary(void) { return ""; }

static void stub_coming_soon_cb(void) {
    refresh_toast_ensure();
    refresh_toast_show("Coming soon", THEME_TEXT);
    // Auto-clear after 2s (piggybacks the existing RT_OK autoclear path).
    refresh_toast_state      = RT_OK;
    refresh_toast_started_ms = lv_tick_get();
}

// ---- Species Catalog (Task 2.2) ----
static const char* catalog_secondary(void) {
    static char buf[8];
    snprintf(buf, sizeof(buf), "%u/13", (unsigned)pet_discovered_count());
    return buf;
}

static void drill_catalog(void) {
    ui_show_screen(SCREEN_CATALOG);
}

// ---- Achievements (Task 2.3) ----
static const char* achievements_secondary(void) {
    static char buf[8];
    snprintf(buf, sizeof(buf), "%u/%u",
             (unsigned)achievements_unlocked_count(),
             (unsigned)ACHIEVEMENTS_COUNT);
    return buf;
}

static void drill_achievements(void) {
    ui_show_screen(SCREEN_ACHIEVEMENTS);
}

// ---- Vacation Mode (Task 1.5) ----
// Toggles vacation on/off and shows a brief toast confirming the change.
// While active, care-stat decay is paused (pet.cpp advances the decay
// anchors to now each tick). Auto-expires after 14 days.

static const char* vacation_secondary(void) {
    if (!pet_is_vacation_active()) return "";
    static char buf[16];
    snprintf(buf, sizeof(buf), "%ud left", (unsigned)pet_vacation_days_remaining());
    return buf;
}

static void vacation_toggle_cb(void) {
    bool was_active = pet_is_vacation_active();
    pet_set_vacation(!was_active);
    refresh_toast_ensure();
    refresh_toast_show(was_active ? "Vacation off" : "Vacation on", THEME_TEXT);
    refresh_toast_state      = RT_OK;
    refresh_toast_started_ms = lv_tick_get();
    // Refresh all secondary labels immediately so "Nd left" / empty shows
    // without waiting for the next menu_screen_tick().
    menu_screen_refresh_secondary_labels();
}

// ---- Speech bubbles toggle (P1-11) ----

static const char* speech_secondary(void) {
    return settings::speech_enabled() ? "on" : "off";
}

static void speech_toggle_cb(void) {
    bool was_on = settings::speech_enabled();
    settings::set_speech_enabled(!was_on);
    refresh_toast_ensure();
    refresh_toast_show(was_on ? "Speech off" : "Speech on", THEME_TEXT);
    refresh_toast_state      = RT_OK;
    refresh_toast_started_ms = lv_tick_get();
    menu_screen_refresh_secondary_labels();
}

// 15-item menu across 3 pages of 6/6/3.
// Every row carries a 48×48 Lucide glyph rendered into
// RGB565A8 (white on transparent) and bundled in icons.h.
static const MenuItem MENU_ITEMS[] = {
    // Page 1 — daily
    { "Pet info",         petinfo_secondary,     drill_petinfo,        &icon_paw_dsc       },
    { "Brightness",       brightness_secondary,  drill_brightness,     &icon_sun_dsc       },
    { "WiFi",             wifi_secondary,        drill_wifi,           &icon_wifi_lg_dsc   },
    { "Bluetooth",        bluetooth_secondary,   drill_bluetooth,      &icon_bluetooth_dsc },
    { "Splash picker",    splash_pick_secondary, drill_splash_pick,    &icon_grid_dsc      },
    { "Refresh now",      refresh_secondary,     drill_refresh,        &icon_refresh_dsc   },
    // Page 2 — pet / collection
    { "Species Catalog",  catalog_secondary,     drill_catalog,        &icon_grid_dsc      },
    { "Achievements",     achievements_secondary, drill_achievements,  &icon_info_dsc      },
    { "Retirement Home",  retirement_secondary,  retirement_open_cb,   &icon_paw_dsc       },
    { "Rename pet",       rename_secondary,      drill_rename,         &icon_paw_dsc       },
    { "Vacation Mode",    vacation_secondary,    vacation_toggle_cb,   &icon_sun_dsc       },
    // Page 3 — system
    { "Speech bubbles",   speech_secondary,      speech_toggle_cb,     &icon_info_dsc      },
    { "About",            about_secondary,       drill_about,          &icon_info_dsc      },
    { "Factory reset",    freset_secondary,      drill_freset,         &icon_alert_dsc     },
};
static const int MENU_ITEMS_COUNT = sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]);
static_assert(MENU_ITEMS_COUNT <= MENU_ITEM_MAX,
              "MENU_ITEM_MAX must be >= MENU_ITEMS_COUNT");

// ----- Gallery formatting helpers (ported verbatim from pet_screen.cpp) -----
static void format_uptime(uint32_t sec, char* buf, size_t len) {
    uint32_t days  = sec / 86400;
    uint32_t hours = (sec % 86400) / 3600;
    uint32_t mins  = (sec % 3600) / 60;
    if (days > 0)       snprintf(buf, len, "%lud %luh %lum", (unsigned long)days, (unsigned long)hours, (unsigned long)mins);
    else if (hours > 0) snprintf(buf, len, "%luh %lum",      (unsigned long)hours, (unsigned long)mins);
    else                snprintf(buf, len, "%lum",           (unsigned long)mins);
}

static const char* bluetooth_secondary(void) {
    switch (ble_get_state()) {
        case BLE_STATE_CONNECTED:    return "Connected";
        case BLE_STATE_ADVERTISING:  return "Advertising...";
        case BLE_STATE_DISCONNECTED: return "Disconnected";
        default: return "";
    }
}

static void refresh_cursor_visual(void) {
    int page = (item_count > 0) ? (menu_cursor / GRID_PAGE_TILES) : 0;
    for (int i = 0; i < GRID_PAGE_TILES; i++) {
        if (!tile_obj[i]) continue;
        int idx = page * GRID_PAGE_TILES + i;
        bool focused = (idx == menu_cursor);
        lv_obj_set_style_border_color(tile_obj[i], THEME_ACCENT, 0);
        lv_obj_set_style_border_width(tile_obj[i], focused ? 3 : 0, 0);
    }
}

static void show_list_view(void) {
    // PSRAM hygiene — drop canvas widgets that reference thumb buffers
    // BEFORE freeing those buffers.
    if (current_view == MENU_VIEW_SPLASHPICK && splash_pick_obj) {
        // Delete the canvas widget first so its buffer pointer is dropped
        // BEFORE we free the underlying PSRAM. Rebuilt on the next drill.
        lv_obj_delete(splash_pick_obj);
        splash_pick_obj = nullptr;
        sp_canvas      = nullptr;
        sp_left_zone   = nullptr;
        sp_right_zone  = nullptr;
        sp_name_lbl    = nullptr;
        sp_counter_lbl = nullptr;
        sp_set_btn     = nullptr;
        sp_set_btn_lbl = nullptr;
        sp_free_buf();
    }
    current_view = MENU_VIEW_LIST;
    ui_status_bar_set_title("MENU");
    ui_status_bar_set_back_cb(nullptr);
    if (about_obj)      lv_obj_add_flag(about_obj, LV_OBJ_FLAG_HIDDEN);
    if (brightness_obj) lv_obj_add_flag(brightness_obj, LV_OBJ_FLAG_HIDDEN);
    if (petinfo_obj)    lv_obj_add_flag(petinfo_obj, LV_OBJ_FLAG_HIDDEN);
    if (wifi_obj)       lv_obj_add_flag(wifi_obj, LV_OBJ_FLAG_HIDDEN);
    if (bt_obj)         lv_obj_add_flag(bt_obj, LV_OBJ_FLAG_HIDDEN);
    if (freset_obj)     lv_obj_add_flag(freset_obj, LV_OBJ_FLAG_HIDDEN);
    if (grid_obj)       lv_obj_clear_flag(grid_obj, LV_OBJ_FLAG_HIDDEN);
    if (bt_subview)     lv_obj_add_flag(bt_subview, LV_OBJ_FLAG_HIDDEN);
    touch_wait_release();
}

// After drilling from a tap-driven PRESSED handler we'd leave LVGL's
// indev thinking the just-pressed row is still "pressed". The next touch
// could then route back to it instead of the new geometric target.
// wait_release() forgets that pressed object and suppresses events until
// the finger lifts, guaranteeing a clean press for the next tap.
static void touch_wait_release(void) {
    lv_indev_t* indev = lv_indev_active();
    if (indev) lv_indev_wait_release(indev);
}


static void bt_refresh_labels(void) {
    if (!bt_obj) return;
    lv_label_set_text(bt_lbl_name, ble_get_device_name());
    lv_label_set_text(bt_lbl_host, (ble_get_state() == BLE_STATE_CONNECTED) ? "paired" : "(none paired)");
    lv_label_set_text(bt_lbl_mac, ble_get_mac_address());

    // Connection-status line: green = paired, amber = advertising, grey = idle.
    if (bt_lbl_conn && bt_status_dot) {
        lv_color_t col; const char* txt;
        switch (ble_get_state()) {
            case BLE_STATE_CONNECTED:   col = THEME_GREEN; txt = "Connected";     break;
            case BLE_STATE_ADVERTISING: col = THEME_AMBER; txt = "Advertising..."; break;
            default:                    col = THEME_DIM;   txt = "Disconnected";  break;
        }
        lv_obj_set_style_bg_color(bt_status_dot, col, 0);
        lv_label_set_text(bt_lbl_conn, txt);
        lv_obj_set_style_text_color(bt_lbl_conn, col, 0);
    }
}

static void bt_apply_visibility(void) {
    bool on = settings::radio_ble_enabled();
    if (bt_content) (on ? lv_obj_clear_flag : lv_obj_add_flag)(bt_content, LV_OBJ_FLAG_HIDDEN);
    if (bt_offmsg)  (on ? lv_obj_add_flag  : lv_obj_clear_flag)(bt_offmsg, LV_OBJ_FLAG_HIDDEN);
}

static void bt_switch_cb(lv_event_t* e) {
    bool on = lv_obj_has_state((lv_obj_t*)lv_event_get_target(e), LV_STATE_CHECKED);
    radio::set_ble(on);
    bt_apply_visibility();
    menu_screen_refresh_secondary_labels();
}

static void build_bt_view(void) {
    if (bt_obj) return;
    const BoardCaps& caps = board_caps();
    int sbh = theme_status_bar_h();

    bt_obj = lv_obj_create(root_obj);
    lv_obj_remove_style_all(bt_obj);
    lv_obj_set_size(bt_obj, caps.width, caps.height);
    lv_obj_set_style_bg_color(bt_obj, THEME_BG, 0);
    lv_obj_set_style_bg_opa(bt_obj, LV_OPA_COVER, 0);
    lv_obj_add_flag(bt_obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(bt_obj, LV_OBJ_FLAG_SCROLLABLE);

    // Switch row
    lv_obj_t* swrow = lv_obj_create(bt_obj);
    lv_obj_remove_style_all(swrow);
    lv_obj_set_size(swrow, caps.width - 40, 58);
    lv_obj_set_pos(swrow, 20, sbh + SP_S);
    lv_obj_set_style_bg_color(swrow, THEME_PANEL, 0);
    lv_obj_set_style_bg_opa(swrow, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(swrow, 12, 0);
    lv_obj_clear_flag(swrow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* swlbl = lv_label_create(swrow);
    lv_label_set_text(swlbl, "Bluetooth");
    lv_obj_set_style_text_font(swlbl, theme_font_display_s(), 0);
    lv_obj_set_style_text_color(swlbl, THEME_TEXT, 0);
    lv_obj_align(swlbl, LV_ALIGN_LEFT_MID, 16, 0);
    bt_sw = lv_switch_create(swrow);
    lv_obj_set_size(bt_sw, 64, 34);
    lv_obj_align(bt_sw, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_bg_color(bt_sw, THEME_BAR_BG, 0);
    lv_obj_set_style_bg_color(bt_sw, THEME_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(bt_sw, THEME_TEXT, LV_PART_KNOB);
    lv_obj_add_event_cb(bt_sw, bt_switch_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    // Note
    lv_obj_t* note = lv_label_create(bt_obj);
    lv_label_set_text(note, "Turning Bluetooth on switches WiFi off.");
    lv_obj_set_style_text_font(note, theme_font_label(), 0);
    lv_obj_set_style_text_color(note, THEME_DIM, 0);
    lv_obj_set_pos(note, 22, sbh + SP_S + 58 + SP_XS);

    // Content container (info rows)
    int content_y = sbh + SP_S + 58 + SP_XS + 26 + SP_S;
    bt_content = lv_obj_create(bt_obj);
    lv_obj_remove_style_all(bt_content);
    lv_obj_set_size(bt_content, caps.width, caps.height - content_y);
    lv_obj_set_pos(bt_content, 0, content_y);
    lv_obj_set_style_bg_opa(bt_content, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(bt_content, LV_OBJ_FLAG_SCROLLABLE);

    // Grouped info card (matches the switch row + tile aesthetic): a colored
    // connection-status line then the Name/Host/MAC rows. The old "Status" row
    // is folded into the status line.
    const int card_x = 20;
    const int card_w = caps.width - 40;
    const int LINE_H = 40, LABEL_W = 110;
    const int rows_top = 44;
    const int card_h = rows_top + 3 * LINE_H + SP_M;
    int card_y = ((caps.height - content_y) - card_h) / 2;
    if (card_y < 0) card_y = 0;

    bt_card = lv_obj_create(bt_content);
    lv_obj_remove_style_all(bt_card);
    lv_obj_set_size(bt_card, card_w, card_h);
    lv_obj_set_pos(bt_card, card_x, card_y);
    lv_obj_set_style_bg_color(bt_card, THEME_PANEL, 0);
    lv_obj_set_style_bg_opa(bt_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bt_card, 12, 0);
    lv_obj_clear_flag(bt_card, LV_OBJ_FLAG_SCROLLABLE);

    bt_status_dot = lv_obj_create(bt_card);
    lv_obj_remove_style_all(bt_status_dot);
    lv_obj_set_size(bt_status_dot, 12, 12);
    lv_obj_set_style_radius(bt_status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(bt_status_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bt_status_dot, THEME_DIM, 0);
    lv_obj_align(bt_status_dot, LV_ALIGN_TOP_LEFT, SP_M, 18);
    bt_lbl_conn = lv_label_create(bt_card);
    lv_obj_set_style_text_font(bt_lbl_conn, theme_font_body(), 0);
    lv_obj_set_style_text_color(bt_lbl_conn, THEME_TEXT, 0);
    lv_label_set_text(bt_lbl_conn, "Disconnected");
    lv_obj_align(bt_lbl_conn, LV_ALIGN_TOP_LEFT, SP_M + 12 + SP_S, 12);

    int y = rows_top;
    int VALUE_W = card_w - SP_M - LABEL_W - SP_M;
    auto add_row = [&](lv_obj_t** out, const char* label) {
        lv_obj_t* l = lv_label_create(bt_card);
        lv_obj_set_style_text_font(l, theme_font_body(), 0);
        lv_obj_set_style_text_color(l, THEME_DIM, 0);
        lv_label_set_text(l, label);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, SP_M, y + 4);
        lv_obj_t* v = lv_label_create(bt_card);
        lv_obj_set_style_text_font(v, theme_font_body(), 0);
        lv_obj_set_style_text_color(v, THEME_TEXT, 0);
        lv_obj_set_width(v, VALUE_W);
        lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -SP_M, y + 4);
        *out = v; y += LINE_H;
    };
    add_row(&bt_lbl_name,   "Name");
    add_row(&bt_lbl_host,   "Host");
    add_row(&bt_lbl_mac,    "MAC");

    // Off-state message
    bt_offmsg = lv_label_create(bt_obj);
    lv_label_set_text(bt_offmsg, "Bluetooth is off.");
    lv_obj_set_style_text_font(bt_offmsg, theme_font_body(), 0);
    lv_obj_set_style_text_color(bt_offmsg, THEME_DIM, 0);
    lv_obj_set_style_text_align(bt_offmsg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(bt_offmsg, LV_ALIGN_CENTER, 0, sbh / 2 + 20);
    lv_obj_add_flag(bt_offmsg, LV_OBJ_FLAG_HIDDEN);
}

static void drill_bluetooth(void) {
    build_bt_view();
    bt_refresh_labels();
    if (bt_sw) {
        if (settings::radio_ble_enabled()) lv_obj_add_state(bt_sw, LV_STATE_CHECKED);
        else                               lv_obj_remove_state(bt_sw, LV_STATE_CHECKED);
    }
    bt_apply_visibility();
    current_view = MENU_VIEW_BLUETOOTH;
    ui_status_bar_set_title("< BLUETOOTH");
    ui_status_bar_set_back_cb(show_list_view);
    if (grid_obj)   lv_obj_add_flag(grid_obj, LV_OBJ_FLAG_HIDDEN);
    if (bt_obj)     lv_obj_clear_flag(bt_obj, LV_OBJ_FLAG_HIDDEN);
    if (bt_subview) lv_obj_add_flag(bt_subview, LV_OBJ_FLAG_HIDDEN);
    touch_wait_release();
}

// Forward decl for grid paging (grid_refill_page calls refresh_cursor_visual which is defined above).
static void grid_refill_page(int page);

// Tap handler: each tile carries its absolute MENU_ITEMS index in user_data.
static void tile_tap_cb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t*)lv_event_get_target(e));
    if (idx < 0 || idx >= MENU_ITEMS_COUNT) return;
    menu_cursor = idx;
    refresh_cursor_visual();
    if (MENU_ITEMS[idx].drill_fn) MENU_ITEMS[idx].drill_fn();
}

static void grid_on_page_change(int page) { grid_refill_page(page); }

static void build_grid_view(void) {
    const BoardCaps& caps = board_caps();
    item_count = MENU_ITEMS_COUNT;

    grid_obj = lv_obj_create(root_obj);
    lv_obj_remove_style_all(grid_obj);
    lv_obj_set_size(grid_obj, caps.width, caps.height);
    lv_obj_set_style_bg_color(grid_obj, THEME_BG, 0);
    lv_obj_set_style_bg_opa(grid_obj, LV_OPA_COVER, 0);
    lv_obj_clear_flag(grid_obj, LV_OBJ_FLAG_SCROLLABLE);

    int sbh      = theme_status_bar_h();
    int gutter_w = 52;
    int top      = sbh + SP_M;
    int bottom   = 30;                 // room for the dot row
    int gap      = SP_M;
    int area_x   = gutter_w + SP_XS;
    int area_w   = caps.width - 2 * (gutter_w + SP_XS);
    int area_h   = caps.height - top - bottom;
    int tile_w   = (area_w - (GRID_COLS - 1) * gap) / GRID_COLS;
    int tile_h   = (area_h - (GRID_ROWS - 1) * gap) / GRID_ROWS;

    for (int i = 0; i < GRID_PAGE_TILES; i++) {
        int col = i % GRID_COLS;
        int row = i / GRID_COLS;
        int x = area_x + col * (tile_w + gap);
        int y = top    + row * (tile_h + gap);

        lv_obj_t* t = lv_obj_create(grid_obj);
        lv_obj_remove_style_all(t);
        lv_obj_set_size(t, tile_w, tile_h);
        lv_obj_set_pos(t, x, y);
        lv_obj_set_style_bg_color(t, THEME_PANEL, 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(t, 12, 0);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t, tile_tap_cb, LV_EVENT_SHORT_CLICKED, nullptr);
        // Vertical flex stack (icon -> label -> status), centered. Flex
        // guarantees the children never overlap regardless of 1- or 2-line
        // labels — the old bottom-anchored layout let 2-line labels collide
        // with the icon.
        lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(t, SP_XS, 0);
        lv_obj_set_style_pad_row(t, SP_XS, 0);
        tile_obj[i] = t;

        lv_obj_t* img = lv_image_create(t);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
        // Icons are native 48px Lucide glyphs; scale to ~34px so a 2-line
        // label + status fit the tile height.
        lv_image_set_scale(img, 180);   // 180/256 ≈ 70% of 48 ≈ 34px
        tile_icon[i] = img;

        lv_obj_t* p = lv_label_create(t);
        lv_obj_set_style_text_font(p, theme_font_body(), 0);
        lv_obj_set_style_text_color(p, THEME_TEXT, 0);
        lv_obj_set_width(p, tile_w - 2 * SP_XS);
        lv_label_set_long_mode(p, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(p, LV_TEXT_ALIGN_CENTER, 0);
        tile_primary[i] = p;

        lv_obj_t* s = lv_label_create(t);
        lv_obj_set_style_text_font(s, theme_font_label(), 0);
        lv_obj_set_style_text_color(s, THEME_DIM, 0);
        lv_obj_set_width(s, tile_w - 2 * SP_XS);
        lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
        tile_status[i] = s;
    }

    paged_nav_create(&grid_nav, grid_obj, gutter_w, top, bottom, grid_on_page_change);
    int pages = (item_count + GRID_PAGE_TILES - 1) / GRID_PAGE_TILES;
    paged_nav_set_page_count(&grid_nav, pages);
    grid_refill_page(0);
}

// Refill the 6 reusable tiles from MENU_ITEMS for the given page. Tiles past
// the end of the list are hidden. One bounded redraw — no scroll, no anim.
static void grid_refill_page(int page) {
    for (int i = 0; i < GRID_PAGE_TILES; i++) {
        int idx = page * GRID_PAGE_TILES + i;
        if (idx >= item_count) {
            lv_obj_add_flag(tile_obj[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(tile_obj[i], LV_OBJ_FLAG_HIDDEN);
        if (MENU_ITEMS[idx].icon) {
            lv_image_set_src(tile_icon[i], MENU_ITEMS[idx].icon);
            lv_obj_clear_flag(tile_icon[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(tile_icon[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_label_set_text(tile_primary[i], MENU_ITEMS[idx].primary);
        lv_label_set_text(tile_status[i],
            MENU_ITEMS[idx].secondary_fn ? MENU_ITEMS[idx].secondary_fn() : "");
        lv_obj_set_user_data(tile_obj[i], (void*)(intptr_t)idx);
    }
    refresh_cursor_visual();
}


void menu_screen_init(lv_obj_t* root, lv_obj_t* bluetooth_subview) {
    root_obj  = root;
    bt_subview = bluetooth_subview;
    init_menu_icons();
    build_grid_view();
    show_list_view();
    refresh_cursor_visual();
}

void menu_screen_refresh_secondary_labels(void) {
    // All 6 tiles always exist; only the current page's data is loaded into
    // them, so refill it to refresh status lines (WiFi on/off, Vacation, etc.).
    if (grid_obj && current_view == MENU_VIEW_LIST)
        grid_refill_page(grid_nav.page);
}

void menu_screen_show(void) {
    menu_cursor = 0;
    show_list_view();
    paged_nav_goto(&grid_nav, 0);
    refresh_cursor_visual();
    menu_screen_refresh_secondary_labels();
}

void menu_screen_tick(void) {
    uint32_t now = lv_tick_get();

    // Splash picker carousel — advance the frame, and if parked on the
    // "Auto" slot, also rotate through species every SP_AUTO_ROTATE_MS.
    if (current_view == MENU_VIEW_SPLASHPICK && sp_canvas_buf) {
        bool need_render = false;
        if (sp_browse_idx == 0 && now - sp_auto_rotate_ms >= SP_AUTO_ROTATE_MS) {
            sp_auto_rotate_ms = now;
            sp_auto_species   = (sp_auto_species + 1) % 13;
            sp_frame_idx      = 0;
            need_render       = true;
        }
        if (now - sp_last_frame_ms >= SP_FRAME_MS) {
            sp_last_frame_ms = now;
            sp_frame_idx++;
            need_render = true;
        }
        if (need_render) sp_render_current();
    }

    if (refresh_toast_state == RT_REQUESTED) {
        if (app_last_apply_ms() != refresh_toast_baseline_apply) {
            refresh_toast_show("Updated", THEME_GREEN);
            refresh_toast_state      = RT_OK;
            refresh_toast_started_ms = now;
        } else if (now - refresh_toast_started_ms >= REFRESH_TIMEOUT_MS) {
            refresh_toast_show("Failed", THEME_AMBER);
            refresh_toast_state      = RT_FAIL;
            refresh_toast_started_ms = now;
        }
    } else if (refresh_toast_state == RT_OK || refresh_toast_state == RT_FAIL) {
        if (now - refresh_toast_started_ms >= REFRESH_AUTOCLEAR_MS) {
            refresh_toast_hide();
            refresh_toast_state = RT_IDLE;
        }
    }

    // WiFi Reconnect action state machine — only fires when the user is on
    // the WiFi drill and has tapped Reconnect.
    if (current_view == MENU_VIEW_WIFI && wifi_action_state == WAS_RECONNECTING) {
        if (WiFi.isConnected()) {
            wifi_action_state = WAS_OK;
            wifi_action_started_ms = now;
            if (wifi_lbl_action) {
                lv_label_set_text(wifi_lbl_action, "Connected");
                lv_obj_set_style_text_color(wifi_lbl_action, THEME_GREEN, 0);
            }
            wifi_refresh_labels();
        } else if (now - wifi_action_started_ms >= WIFI_RECONNECT_TIMEOUT_MS) {
            wifi_action_state = WAS_FAIL;
            wifi_action_started_ms = now;
            if (wifi_lbl_action) {
                lv_label_set_text(wifi_lbl_action, "Failed");
                lv_obj_set_style_text_color(wifi_lbl_action, THEME_AMBER, 0);
            }
        }
    } else if ((wifi_action_state == WAS_OK || wifi_action_state == WAS_FAIL) &&
               (now - wifi_action_started_ms >= 2000)) {
        if (wifi_lbl_action) lv_label_set_text(wifi_lbl_action, "");
        wifi_action_state = WAS_IDLE;
    }

    // Factory reset hold-to-confirm progress.
    if (current_view == MENU_VIEW_FRESET && freset_press_started_ms != 0) {
        uint32_t held = now - freset_press_started_ms;
        if (held >= FRESET_HOLD_MS) {
            factory_reset_perform();   // does not return
        } else if (freset_bar) {
            lv_bar_set_value(freset_bar, (int32_t)held, LV_ANIM_OFF);
        }
    }
}

void menu_screen_reset_view(void) {
    show_list_view();
}

bool menu_screen_on_button(ui_btn_t btn) {
    if (current_view != MENU_VIEW_LIST) {
        if (btn == UI_BTN_MID) {
            show_list_view();
            Serial.println("menu: Mid -> back to list");
            return true;
        }
        return false;
    }
    switch (btn) {
        case UI_BTN_LEFT:
            if (item_count <= 0) return true;
            menu_cursor = (menu_cursor + 1) % item_count;
            // Advancing past the last tile on a page flips the page.
            paged_nav_goto(&grid_nav, menu_cursor / GRID_PAGE_TILES);
            refresh_cursor_visual();
            Serial.printf("menu: L -> focus=%d\n", menu_cursor);
            return true;
        case UI_BTN_RIGHT:
            if (menu_cursor < 0 || menu_cursor >= item_count) return true;
            if (MENU_ITEMS[menu_cursor].drill_fn) {
                Serial.printf("menu: R -> drill %d (%s)\n",
                    menu_cursor, MENU_ITEMS[menu_cursor].primary);
                MENU_ITEMS[menu_cursor].drill_fn();
            }
            return true;
        case UI_BTN_MID:
            return false;   // let dispatcher cycle screens
    }
    return false;
}
