#include "pet_screen.h"
#include "pet_speech.h"
#include "pet.h"
#include "streak.h"
#include "recap.h"
#include "ui.h"
#include "ui_panels.h"
#include "theme.h"
#include "splash.h"
#include "keyboard_overlay.h"
#include "settings.h"
#include "hal/board_caps.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>

// Fonts come in via theme.h's theme_font_* helpers.

// Lucide icon data lives in icons.h. Descriptors are owned here so they
// stay alive for the lifetime of the screen.
#include "icons.h"
#include "ui_icons.h"
static lv_image_dsc_t icon_utensils_dsc;
static lv_image_dsc_t icon_party_dsc;
static lv_image_dsc_t icon_heart_dsc;
static lv_image_dsc_t icon_sparkles_dsc;
static lv_image_dsc_t icon_flame_dsc;

// ============== Recap modal (Task 3.2) ==============
// Full-screen modal, parented to lv_scr_act() so it floats above everything.
// Destroyed on dismiss; nullptr when not visible.
static lv_obj_t* recap_modal_obj = nullptr;

// When a hatch event fires on a new day, the HATCHED handler navigates to
// SCREEN_PET (which calls maybe_show_recap) before showing the naming keyboard.
// Setting this flag prevents the recap from stacking under the keyboard.
// The recap is deferred to the next session instead.
static bool g_suppress_next_recap = false;

// Dismiss handler — tap-anywhere or Done button.
static void recap_dismiss_cb(lv_event_t* e) {
    (void)e;
    if (!recap_modal_obj) return;
    lv_obj_delete(recap_modal_obj);
    recap_modal_obj = nullptr;
    recap_mark_shown();
    Serial.println("[recap] modal dismissed");
}

// Build and show the recap modal using the given snapshot.
// `snap` may be nullptr (e.g. when today has no history yet) — in that case
// we still show the title with zero stats.
static void show_recap_modal(const DailySnapshot* snap) {
    // Tear down any existing modal.
    if (recap_modal_obj) {
        lv_obj_delete(recap_modal_obj);
        recap_modal_obj = nullptr;
    }

    const BoardCaps& caps = board_caps();
    bool large = caps.height >= 460;
    const lv_font_t* font_title  = theme_font_display_s();   // e.g. Styrene 28
    const lv_font_t* font_body   = theme_font_body();        // e.g. Styrene 24
    const lv_font_t* font_lbl    = theme_font_label();       // e.g. Styrene 16

    // ----- Full-screen dark overlay -----
    recap_modal_obj = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(recap_modal_obj);
    lv_obj_set_size(recap_modal_obj, caps.width, caps.height);
    lv_obj_set_pos(recap_modal_obj, 0, 0);
    lv_obj_set_style_bg_color(recap_modal_obj, THEME_BG, 0);
    lv_obj_set_style_bg_opa(recap_modal_obj, LV_OPA_90, 0);
    lv_obj_add_flag(recap_modal_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(recap_modal_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(recap_modal_obj);

    // Tap anywhere on the overlay to dismiss.
    lv_obj_add_event_cb(recap_modal_obj, recap_dismiss_cb, LV_EVENT_CLICKED, nullptr);

    // ----- Centered card -----
    const int card_margin = SP_XL;
    const int card_w = caps.width - 2 * card_margin;
    // Card height: dynamic — laid out around content, sized after all rows placed.
    const int card_x = card_margin;

    lv_obj_t* card = lv_obj_create(recap_modal_obj);
    lv_obj_remove_style_all(card);
    // Temporary position; y will be re-centered after we know the final height.
    lv_obj_set_pos(card, card_x, 0);
    lv_obj_set_size(card, card_w, caps.height);  // oversized temporarily
    lv_obj_set_style_bg_color(card, THEME_PANEL, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_border_color(card, THEME_ACCENT, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    // Stop tap propagation from reaching the overlay dismiss handler.
    lv_obj_add_event_cb(card, [](lv_event_t* e){ (void)e; }, LV_EVENT_CLICKED, nullptr);

    int y = SP_M;
    const int pad_x = SP_M;
    const int lbl_w = card_w - 2 * pad_x;

    // ----- Title: "Yesterday with <name>" -----
    {
        const Pet* p = pet_current();
        char title_buf[32];
        const char* pet_name = (p && p->name[0]) ? p->name : "your pet";
        snprintf(title_buf, sizeof(title_buf), "Yesterday with %s", pet_name);

        lv_obj_t* lbl = lv_label_create(card);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, lbl_w);
        lv_obj_set_style_text_font(lbl, font_title, 0);
        lv_obj_set_style_text_color(lbl, THEME_ACCENT, 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(lbl, title_buf);
        lv_obj_set_pos(lbl, pad_x, y);
        y += (large ? 36 : 28) + SP_S;
    }

    // ----- Thin separator -----
    {
        lv_obj_t* sep = lv_obj_create(card);
        lv_obj_remove_style_all(sep);
        lv_obj_set_size(sep, lbl_w, 1);
        lv_obj_set_pos(sep, pad_x, y);
        lv_obj_set_style_bg_color(sep, THEME_ACCENT, 0);
        lv_obj_set_style_bg_opa(sep, LV_OPA_40, 0);
        y += 1 + SP_S;
    }

    // ----- Stat lines -----
    // Helper lambda: add a stat row label.
    auto add_stat = [&](const char* text, lv_color_t color) {
        lv_obj_t* lbl = lv_label_create(card);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, lbl_w);
        lv_obj_set_style_text_font(lbl, font_body, 0);
        lv_obj_set_style_text_color(lbl, color, 0);
        lv_label_set_text(lbl, text);
        lv_obj_set_pos(lbl, pad_x, y);
        y += (large ? 28 : 24) + SP_XS;
    };

    if (snap) {
        char buf[48];

        snprintf(buf, sizeof(buf), "XP gained: %lu", (unsigned long)snap->xp_gained);
        add_stat(buf, THEME_TEXT);

        snprintf(buf, sizeof(buf), "Care actions: %u", (unsigned)snap->care_actions);
        add_stat(buf, THEME_TEXT);

        snprintf(buf, sizeof(buf), "Care streak: %u days", (unsigned)snap->care_streak_end);
        add_stat(buf, snap->care_streak_end > 0 ? THEME_AMBER : THEME_DIM);

        snprintf(buf, sizeof(buf), "Use streak:  %u days", (unsigned)snap->usage_streak_end);
        add_stat(buf, snap->usage_streak_end > 0 ? THEME_AMBER : THEME_DIM);

        if (snap->achievements_unlocked > 0) {
            snprintf(buf, sizeof(buf), "Achievements: %u new!", (unsigned)snap->achievements_unlocked);
            add_stat(buf, THEME_PURPLE);
        }

        if (snap->shinies_seen > 0) {
            snprintf(buf, sizeof(buf), "Shinies: %u!", (unsigned)snap->shinies_seen);
            add_stat(buf, THEME_ACCENT);
        }
    } else {
        // No snapshot yet — first day.
        add_stat("No data yet — check back tomorrow!", THEME_DIM);
    }

    y += SP_XS;

    // ----- Signature speech bubble line -----
    {
        uint32_t salt = (uint32_t)(millis() / 60000);
        const char* quote = pet_speech_pick(SPEECH_GREETING, salt);
        if (quote && quote[0]) {
            lv_obj_t* lbl = lv_label_create(card);
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(lbl, lbl_w);
            lv_obj_set_style_text_font(lbl, font_lbl, 0);
            lv_obj_set_style_text_color(lbl, THEME_DIM, 0);
            lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);

            // Wrap quote in quotation marks.
            char quote_buf[80];
            snprintf(quote_buf, sizeof(quote_buf), "\"%s\"", quote);
            lv_label_set_text(lbl, quote_buf);
            lv_obj_set_pos(lbl, pad_x, y);
            y += (large ? 48 : 40);   // two lines of label font
        }
    }

    // ----- Done button -----
    const int btn_h = large ? 40 : 32;
    {
        lv_obj_t* btn = lv_button_create(card);
        lv_obj_set_size(btn, lbl_w, btn_h);
        lv_obj_set_pos(btn, pad_x, y);
        lv_obj_set_style_bg_color(btn, THEME_ACCENT, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_add_event_cb(btn, recap_dismiss_cb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* btn_lbl = lv_label_create(btn);
        lv_label_set_text(btn_lbl, "Done");
        lv_obj_set_style_text_font(btn_lbl, font_body, 0);
        lv_obj_set_style_text_color(btn_lbl, THEME_TEXT, 0);
        lv_obj_align(btn_lbl, LV_ALIGN_CENTER, 0, 0);
    }

    // ----- Dynamic card sizing: fit exactly around content -----
    {
        const int card_h = y + btn_h + SP_M;
        const int max_card_h = caps.height - 2 * card_margin;
        const int final_card_h = (card_h < max_card_h) ? card_h : max_card_h;
        const int card_y = (caps.height - final_card_h) / 2;
        lv_obj_set_size(card, card_w, final_card_h);
        lv_obj_set_pos(card, card_x, card_y);
    }

    Serial.printf("[recap] modal shown (snap=%s)\n", snap ? "yes" : "no");
}

// Check if the recap should be shown and do so. Called from pet_screen_show().
static void maybe_show_recap(void) {
    if (g_suppress_next_recap) {
        g_suppress_next_recap = false;
        recap_mark_shown();  // skip today's recap entirely; defer to next session
        return;
    }
    if (!recap_should_show()) return;
    const DailySnapshot* snap = recap_yesterday();
    show_recap_modal(snap);
}

static void init_pet_icons(void) {
    ui_icon_init_rgb565a8(&icon_utensils_dsc, ICON_UTENSILS_W, ICON_UTENSILS_H, icon_utensils_data);
    ui_icon_init_rgb565a8(&icon_party_dsc,    ICON_PARTY_W,    ICON_PARTY_H,    icon_party_data);
    ui_icon_init_rgb565a8(&icon_heart_dsc,    ICON_HEART_W,    ICON_HEART_H,    icon_heart_data);
    ui_icon_init_rgb565a8(&icon_sparkles_dsc, ICON_SPARKLES_W, ICON_SPARKLES_H, icon_sparkles_data);
    ui_icon_init_rgb565a8(&icon_flame_dsc,    ICON_FLAME_W,    ICON_FLAME_H,    icon_flame_data);
}

// Module-owned widget pointers. `root` is the pet_container passed by
// ui.cpp's init_pet_screen() — we build everything inside it.
static lv_obj_t* root = nullptr;

enum pet_view_t { PET_HOME, PET_CARE_DETAIL };
static pet_view_t pet_view = PET_HOME;

// Home sub-view container. detail_obj (declared below) holds the
// PET_CARE_DETAIL widget tree.
static lv_obj_t* home_obj = nullptr;

// Detail view widgets
static lv_obj_t* detail_obj = nullptr;

static lv_obj_t* sprite_canvas = nullptr;

// ----- Care rows (phase 3) -----
static lv_obj_t* care_row[3];           // 0=satiety, 1=spirit, 2=bond (now the make_stat_panel wrapper)
static lv_obj_t* care_bar[3];
static lv_obj_t* care_value_lbl[3];
static lv_obj_t* care_subtitle_lbl[3];  // band-word ("Full"/"Peckish"/"Hungry" etc.)

// Phase 4 — Sat ↔ Spi cursor. Bnd is never cursor-focused (it's
// sprite-tap driven). 0 = Sat, 1 = Spi.
static int pet_cursor = 0;

#define CURSOR_BORDER_WIDTH 4

// Floating "+25" label per action target — hidden by default; shown briefly
// after a successful action, fades up over ~1s.
// Phase 4 indexing: [0] = Sat row, [1] = Spi row, [2] = sprite/Bnd.
static lv_obj_t* care_float_lbl[3];
static uint32_t  care_float_start_ms[3] = {0};
#define CARE_FLOAT_MS 1000

static uint16_t* sprite_canvas_buf = nullptr;
static int g_canvas_w_alloc = 0;   // canvas buffer size (large enough for base_cell)
static int g_base_cell      = 14;  // captured at init from layout breakpoint

// Animation state
static uint16_t sprite_frame_idx = 0;
static uint32_t sprite_last_frame_ms = 0;
#define SPRITE_FRAME_MS  250   // ~4Hz

// Event toast — fires for 2s when pet_screen_on_event is called while
// SCREEN_PET is active. Only rendered on the home view.
static uint32_t toast_start_ms = 0;
static lv_obj_t* toast_lbl = nullptr;
#define TOAST_MS 2000

// Shiny sparkle indicator — lv_image overlay anchored to the sprite canvas
// top-right. Created/destroyed by refresh_shiny_sparkle() so the overlay
// stays in sync after a graduation hatches a new pet (P1-9).
static lv_obj_t* shiny_sparkle_img = nullptr;

// Create-or-destroy the sparkle overlay based on the current pet's is_shiny.
// Safe to call repeatedly; safe to call when home_obj / sprite_canvas haven't
// been built yet (no-op). Called from build_home_view (initial), pet_screen_show
// (every screen entry), and pet_screen_on_event(PET_EVENT_HATCHED) (graduation
// path that swaps the pet without re-entering the screen).
static void refresh_shiny_sparkle(void) {
    if (!home_obj || !sprite_canvas) return;
    const Pet* p = pet_current();
    bool want = (p && p->is_shiny);
    if (want && !shiny_sparkle_img) {
        shiny_sparkle_img = lv_image_create(home_obj);
        lv_image_set_src(shiny_sparkle_img, &icon_sparkles_dsc);
        lv_obj_align_to(shiny_sparkle_img, sprite_canvas,
                        LV_ALIGN_TOP_RIGHT, -2, 2);
    } else if (!want && shiny_sparkle_img) {
        lv_obj_delete(shiny_sparkle_img);
        shiny_sparkle_img = nullptr;
    }
}

// Home view — pet name label (Task 1.2). Sits above the sprite canvas,
// centered horizontally. Refreshed every second via refresh_status_labels().
static lv_obj_t* name_lbl          = nullptr;

// Vacation banner (Task 1.5). Shown below the name when vacation is active.
// Hidden when vacation is off. Refreshed each second in refresh_status_labels().
static lv_obj_t* vacation_banner_lbl = nullptr;

// Streak row (Task 3.1 / Task 2.7). Single composite flame+count.
// Hidden when composite streak = 0. Updated each second.
static lv_obj_t* streak_row_obj    = nullptr;   // container row — hidden when streak 0
static lv_obj_t* streak_care_lbl   = nullptr;   // composite count (semantically: max(care,usage))

// Care dot (Task 2.7). Single colored dot — worst-of-three stat color.
// Green > 50, Amber > 25, Red otherwise.
static lv_obj_t* care_dot          = nullptr;

// Speech bubble (Task 2.1). Floats above the sprite canvas on the home view.
// Auto-hides after SPEECH_BUBBLE_MS milliseconds. Created hidden; shown by
// show_speech_bubble() and auto-dismissed by pet_screen_tick().
static lv_obj_t*  speech_bubble_lbl         = nullptr;
static uint32_t   speech_bubble_show_until_ms = 0;
#define SPEECH_BUBBLE_MS  5000   // display duration per bubble

// Next speech attempt timestamp (millis). Initialized to 60s after boot so
// the first bubble fires ~1 min in (gives the screen time to settle). The
// greeting shown on screen-open bypasses this entirely.
static uint32_t g_speech_next_attempt_ms = 0;

// Called when the user confirms a name via the on-hatch or rename keyboard.
// Empty text is ignored (keeps the default species name).
static void on_pet_name_chosen(const char* text, void* user_data) {
    (void)user_data;
    if (!text || !text[0]) return;
    pet_rename(text);
    // Refresh the name label immediately so the change is visible without
    // waiting for the next 1-second label-refresh tick.
    if (name_lbl) {
        const Pet* p = pet_current();
        lv_label_set_text(name_lbl, (p && p->name[0]) ? p->name : "");
    }
}

static const char* toast_text_for(pet_event_t ev) {
    switch (ev) {
        case PET_EVENT_LEVEL_UP:  return "LEVEL UP!";
        case PET_EVENT_EVOLVE:    return "EVOLVED!";
        case PET_EVENT_GRADUATE:  return "GRADUATED!";
        case PET_EVENT_NEGLECTED: return "I need some care...";
        case PET_EVENT_HATCHED:   return "";   // keyboard handles the feedback
    }
    return "";
}

// 1-second cadence for non-sprite label refresh (XP, care stats)
static uint32_t labels_last_refresh_ms = 0;
#define LABEL_REFRESH_MS 1000

// Forward declaration — helpers below need to trigger a label refresh,
// but the implementation lives further down the file.
static void refresh_status_labels(void);

// Apply the 4px amber left-border to the focused row, clear it from the
// other Sat/Spi row. Bnd is never targeted. Only runs in PET_CARE_DETAIL.
static void refresh_cursor_visual(void) {
    if (pet_view != PET_CARE_DETAIL) return;
    for (int i = 0; i < 2; i++) {
        if (!care_row[i]) continue;
        bool focused = (i == pet_cursor);
        lv_obj_set_style_border_color(care_row[i], THEME_AMBER, 0);
        lv_obj_set_style_border_side(care_row[i], LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(care_row[i], focused ? CURSOR_BORDER_WIDTH : 0, 0);
    }
}

// Show the +N float label for action target i (0=Sat, 1=Spi, 2=Bnd).
static void care_show_float(int i) {
    if (i < 0 || i > 2) return;
    if (!care_float_lbl[i]) return;
    care_float_start_ms[i] = lv_tick_get();
    lv_obj_clear_flag(care_float_lbl[i], LV_OBJ_FLAG_HIDDEN);
}

// Show the speech bubble above the sprite with `text` for SPEECH_BUBBLE_MS.
// Gated on settings::speech_enabled() (P1-11). The force entrypoint
// (pet_screen_force_speech_bubble) intentionally bypasses this so the serial
// `speech` test cmd still works for QA.
static void show_speech_bubble(const char* text) {
    if (!speech_bubble_lbl || !text || !text[0]) return;
    if (!settings::speech_enabled()) return;
    lv_label_set_text(speech_bubble_lbl, text);
    lv_obj_clear_flag(speech_bubble_lbl, LV_OBJ_FLAG_HIDDEN);
    speech_bubble_show_until_ms = millis() + SPEECH_BUBBLE_MS;
}

static void hide_speech_bubble(void) {
    if (!speech_bubble_lbl) return;
    lv_obj_add_flag(speech_bubble_lbl, LV_OBJ_FLAG_HIDDEN);
    speech_bubble_show_until_ms = 0;
}

// Flash the existing event-toast widget with an arbitrary string for TOAST_MS.
// Used to surface the reason a tapped action got rejected (cooldown / full).
static void flash_toast(const char* text) {
    if (!toast_lbl) return;
    lv_label_set_text(toast_lbl, text);
    lv_obj_clear_flag(toast_lbl, LV_OBJ_FLAG_HIDDEN);
    toast_start_ms = lv_tick_get();
}

// "On cooldown — wait Xm Ys" / "Already full" feedback when an action returns
// false. Without this the user sees no response at all and assumes the button
// is broken. idx: 0=feed, 1=play, 2=pet.
static void report_rejection(int idx) {
    PetStats s = pet_stats();
    uint8_t stat = (idx == 0) ? s.satiety : (idx == 1) ? s.spirit : s.bond;
    uint32_t cd  = (idx == 0) ? s.feed_cooldown_remaining
                : (idx == 1) ? s.play_cooldown_remaining
                :              s.pet_cooldown_remaining;
    char buf[40];
    if (stat >= 100) {
        snprintf(buf, sizeof(buf), "Already full");
    } else if (cd > 0) {
        uint32_t m = cd / 60;
        uint32_t sec = cd % 60;
        if (m > 0) snprintf(buf, sizeof(buf), "Wait %lum %lus", (unsigned long)m, (unsigned long)sec);
        else       snprintf(buf, sizeof(buf), "Wait %lus", (unsigned long)sec);
    } else {
        snprintf(buf, sizeof(buf), "Not ready");
    }
    flash_toast(buf);
}

static void sprite_tap_cb(lv_event_t* e) {
    (void)e;
    bool ok = pet_pet();
    Serial.printf("pet_screen: sprite tap → %s\n", ok ? "OK" : "REJECT");
    if (ok) {
        refresh_status_labels();
        care_show_float(2);   // Bnd float above sprite
    } else {
        report_rejection(2);
    }
}

// AMOLED-1.8 (2-button) fallback: rows are tappable for the commit action
// when there's no physical R button. user_data carries the row index (0=Sat, 1=Spi).
static void care_row_tap_cb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx > 1) return;
    // Tap = select + act in one gesture. Move cursor first so the visual
    // highlight follows the user's tap regardless of whether the commit
    // succeeds (it can fail on cooldown / full stat).
    pet_cursor = idx;
    refresh_cursor_visual();
    bool ok = false;
    if (idx == 0) {
        ok = pet_feed();
        Serial.printf("pet_screen: tap→Feed → %s\n", ok ? "OK" : "REJECT");
    } else if (idx == 1) {
        ok = pet_play();
        Serial.printf("pet_screen: tap→Play → %s\n", ok ? "OK" : "REJECT");
    }
    if (ok) {
        refresh_status_labels();
        care_show_float(idx);
    } else {
        report_rejection(idx);
    }
}

// Forward-declare view transition helpers so build_home_view can reference them
static void enter_detail_view(void);
static void exit_detail_view(void);

static void build_detail_view(void) {
    const BoardCaps& caps = board_caps();
    bool large = caps.height >= 460;

    detail_obj = lv_obj_create(root);
    lv_obj_remove_style_all(detail_obj);
    lv_obj_set_size(detail_obj, caps.width, caps.height);
    lv_obj_set_pos(detail_obj, 0, 0);
    // See home_obj — opaque so the show/hide flip actually clears the
    // previous view's pixels on CO5300 partial-render.
    lv_obj_set_style_bg_color(detail_obj, THEME_BG, 0);
    lv_obj_set_style_bg_opa(detail_obj, LV_OPA_COVER, 0);
    lv_obj_clear_flag(detail_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(detail_obj, LV_OBJ_FLAG_HIDDEN);   // home is the default

    init_pet_icons();

    int sbh    = theme_status_bar_h();
    int card_h = large ? 130 : 110;
    int gap    = SP_M;

    int y_cursor = sbh + SP_XS + SP_M;

    const char*               pill_text[3] = { "Sat", "Spi", "Bnd" };
    const lv_image_dsc_t*     icon_dsc[3]  = { &icon_utensils_dsc, &icon_party_dsc, &icon_heart_dsc };

    for (int i = 0; i < 3; i++) {
        lv_obj_t* val = nullptr, *pill = nullptr, *bar = nullptr, *sub = nullptr;
        // make_stat_panel adds its panel to `detail_obj` at (L.margin, y_cursor)
        // with width L.content_w — same alignment as USAGE/SESSION cards.
        make_stat_panel(detail_obj, y_cursor, card_h, icon_dsc[i], pill_text[i],
                        &val, &pill, &bar, &sub);

        // The panel returned by make_panel is the immediate parent of `val`.
        // Use it directly as the focus target — no wrapper needed; the
        // cursor border styles render right on the panel's left edge.
        care_row[i]          = lv_obj_get_parent(val);
        care_value_lbl[i]    = val;
        care_bar[i]          = bar;
        care_subtitle_lbl[i] = sub;

        // Pill labels (Sat/Spi/Bnd) hidden — the icons already disambiguate.
        lv_obj_add_flag(pill, LV_OBJ_FLAG_HIDDEN);

        // Focus-border attributes — width gets driven by refresh_cursor_visual.
        lv_obj_set_style_border_color(care_row[i], THEME_AMBER, 0);
        lv_obj_set_style_border_side(care_row[i], LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(care_row[i], 0, 0);

        // Sat/Spi are tappable to commit (Bnd is sprite-tap-driven). T5
        // removes the button_count guard; for now keep it so AMOLED-2.16
        // users still navigate by buttons until the touch rollout lands.
        if (i < 2) {
            lv_obj_add_flag(care_row[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(care_row[i], care_row_tap_cb, LV_EVENT_PRESSED,
                                (void*)(intptr_t)i);
        }

        y_cursor += card_h + gap;
    }

    // Float labels for Sat/Spi — anchored to the card's value column.
    // Bnd float still lives on the home view (anchored to the sprite).
    for (int i = 0; i < 2; i++) {
        care_float_lbl[i] = lv_label_create(care_row[i]);
        lv_label_set_text_fmt(care_float_lbl[i], "+%u", (unsigned)ACTION_REFILL_AMOUNT);
        lv_obj_set_style_text_font(care_float_lbl[i], theme_font_label(), 0);
        lv_obj_set_style_text_color(care_float_lbl[i], THEME_AMBER, 0);
        lv_obj_align_to(care_float_lbl[i], care_value_lbl[i],
                        LV_ALIGN_OUT_RIGHT_TOP, SP_S, 0);
        lv_obj_add_flag(care_float_lbl[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_home_view(void) {
    const BoardCaps& caps = board_caps();
    bool large = caps.height >= 460;

    home_obj = lv_obj_create(root);
    lv_obj_remove_style_all(home_obj);
    lv_obj_set_size(home_obj, caps.width, caps.height);
    lv_obj_set_pos(home_obj, 0, 0);
    // Opaque so CO5300 partial-render properly repaints when we toggle
    // between home and the care-detail sub-view. With LV_OPA_TRANSP we'd
    // depend on every detail-view widget covering its own footprint, which
    // it doesn't — and the result was stale home pixels masking the
    // transition (same class of bug as the menu's bluetooth subview).
    lv_obj_set_style_bg_color(home_obj, THEME_BG, 0);
    lv_obj_set_style_bg_opa(home_obj, LV_OPA_COVER, 0);
    lv_obj_clear_flag(home_obj, LV_OBJ_FLAG_SCROLLABLE);

    int sbh = theme_status_bar_h();
    int y_cursor = sbh + SP_XS;

    // ----- Pet name label (Task 1.2) -----
    // Centered above the sprite, primary text, truncates with ellipsis.
    {
        const Pet* p = pet_current();
        const int margin = SP_XL;  // 20px left/right margin — matches layout L.margin
        name_lbl = lv_label_create(home_obj);
        lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name_lbl, caps.width - 2 * margin);
        // Bumped from theme_font_display_s (Styrene 28) to theme_font_display
        // (Tiempos 56 on 2.16) to make the name the hero element, matching
        // USAGE's "---%" hierarchy and the user's "larger text" feedback.
        lv_obj_set_style_text_font(name_lbl, theme_font_display(), 0);
        lv_obj_set_style_text_color(name_lbl, THEME_TEXT, 0);
        lv_obj_set_style_text_align(name_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(name_lbl, (p && p->name[0]) ? p->name : "");
        lv_obj_align(name_lbl, LV_ALIGN_TOP_MID, 0, y_cursor);
        y_cursor += 20 + SP_XS;  // font_styrene_20 line height ≈ 20px, then 4px gap
    }

    // ----- Vacation banner (Task 1.5) -----
    // Always reserves a fixed row at this y position so the sprite canvas
    // below it doesn't need to shift when vacation is toggled at runtime.
    // The banner is always CREATED (at fixed y), then shown/hidden by
    // refresh_status_labels(). Empty text + hidden when vacation is off.
    {
        const int BANNER_H = 20 + SP_XS;  // matches font_label line height
        vacation_banner_lbl = lv_label_create(home_obj);
        lv_obj_set_style_text_font(vacation_banner_lbl, theme_font_label(), 0);
        lv_obj_set_style_text_color(vacation_banner_lbl, THEME_AMBER, 0);
        lv_obj_set_style_text_align(vacation_banner_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(vacation_banner_lbl, LV_ALIGN_TOP_MID, 0, y_cursor);
        if (pet_is_vacation_active()) {
            char buf[32];
            snprintf(buf, sizeof(buf), "Vacation - %ud left",
                     (unsigned)pet_vacation_days_remaining());
            lv_label_set_text(vacation_banner_lbl, buf);
        } else {
            lv_label_set_text(vacation_banner_lbl, "");
            lv_obj_add_flag(vacation_banner_lbl, LV_OBJ_FLAG_HIDDEN);
        }
        y_cursor += BANNER_H;  // always advance so sprite position is stable
    }

    // ----- Streak row (Task 3.1 / Task 2.7) -----
    // Single composite flame+count (worst-of-care/usage). Hidden when 0.
    {
        const int STREAK_ROW_H = 20 + SP_XS;
        streak_row_obj = lv_obj_create(home_obj);
        lv_obj_remove_style_all(streak_row_obj);
        lv_obj_set_size(streak_row_obj, caps.width, STREAK_ROW_H);
        lv_obj_set_pos(streak_row_obj, 0, y_cursor);
        lv_obj_clear_flag(streak_row_obj, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* flame = lv_image_create(streak_row_obj);
        lv_image_set_src(flame, &icon_flame_dsc);
        lv_obj_align(flame, LV_ALIGN_CENTER, -12, 0);

        streak_care_lbl = lv_label_create(streak_row_obj);
        lv_obj_set_style_text_font(streak_care_lbl, theme_font_label(), 0);
        lv_obj_set_style_text_color(streak_care_lbl, THEME_AMBER, 0);
        lv_label_set_text(streak_care_lbl, "0");
        lv_obj_align_to(streak_care_lbl, flame, LV_ALIGN_OUT_RIGHT_MID, 4, 0);

        lv_obj_add_flag(streak_row_obj, LV_OBJ_FLAG_HIDDEN);
        y_cursor += STREAK_ROW_H;
    }

    // ----- Care dot (Task 2.7) -----
    // Single composite colored dot: green (worst > 50), amber (> 25), red otherwise.
    // Anchored left of the streak row after it is created.
    {
        care_dot = lv_obj_create(home_obj);
        lv_obj_remove_style_all(care_dot);
        lv_obj_set_size(care_dot, 10, 10);
        lv_obj_align(care_dot, LV_ALIGN_TOP_RIGHT, -SP_M, theme_status_bar_h() + SP_S);
        lv_obj_set_style_radius(care_dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(care_dot, THEME_GREEN, 0);
        lv_obj_set_style_bg_opa(care_dot, LV_OPA_COVER, 0);
    }

    // ----- Sprite canvas (centered) -----
    // Bumped from 9→13 on 2.16 so the visible sprite (which fills ~60% of
    // the 20x20 grid) goes from ~108→~156 px tall — closer to the USAGE
    // screen's ---% number prominence.
    g_base_cell = large ? 13 : 7;
    g_canvas_w_alloc = 20 * g_base_cell + 48;
    int canvas_w = g_canvas_w_alloc;
    int canvas_h = g_canvas_w_alloc;
    sprite_canvas_buf = (uint16_t*)heap_caps_malloc(canvas_w * canvas_h * 2, MALLOC_CAP_SPIRAM);
    if (!sprite_canvas_buf) {
        Serial.println("pet_screen: sprite canvas alloc failed (PSRAM exhausted)");
        return;
    }
    sprite_canvas = lv_canvas_create(home_obj);
    lv_canvas_set_buffer(sprite_canvas, sprite_canvas_buf, canvas_w, canvas_h,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_align(sprite_canvas, LV_ALIGN_TOP_MID, 0, y_cursor);
    lv_obj_add_flag(sprite_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sprite_canvas, sprite_tap_cb, LV_EVENT_CLICKED, NULL);

    // Shiny sparkle indicator — created/destroyed via refresh_shiny_sparkle()
    // so post-graduation hatches that flip is_shiny are reflected without a
    // reboot (P1-9).
    shiny_sparkle_img = nullptr;
    refresh_shiny_sparkle();

    y_cursor += canvas_h + SP_M;

    // ----- Bnd float anchored above sprite -----
    care_float_lbl[2] = lv_label_create(home_obj);
    lv_label_set_text_fmt(care_float_lbl[2], "+%u", (unsigned)ACTION_REFILL_AMOUNT);
    lv_obj_set_style_text_font(care_float_lbl[2], theme_font_label(), 0);
    lv_obj_set_style_text_color(care_float_lbl[2], THEME_AMBER, 0);
    lv_obj_align_to(care_float_lbl[2], sprite_canvas, LV_ALIGN_OUT_TOP_MID, 0, -2);
    lv_obj_add_flag(care_float_lbl[2], LV_OBJ_FLAG_HIDDEN);

    // ----- Speech bubble (Task 2.1) -----
    // Comic-strip style: sits at the top of the sprite canvas so it overlaps
    // the sprite's top pixels rather than the name label above. Name stays
    // fully visible at all times. Starts hidden.
    speech_bubble_lbl = lv_label_create(home_obj);
    lv_label_set_text(speech_bubble_lbl, "");
    lv_label_set_long_mode(speech_bubble_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(speech_bubble_lbl, theme_font_label(), 0);
    lv_obj_set_style_text_color(speech_bubble_lbl, THEME_TEXT, 0);
    lv_obj_set_style_text_align(speech_bubble_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(speech_bubble_lbl, THEME_PANEL, 0);
    lv_obj_set_style_bg_opa(speech_bubble_lbl, LV_OPA_90, 0);
    lv_obj_set_style_pad_hor(speech_bubble_lbl, SP_S, 0);
    lv_obj_set_style_pad_ver(speech_bubble_lbl, SP_XS + 2, 0);
    lv_obj_set_style_radius(speech_bubble_lbl, 10, 0);
    // Position: at the top edge of the sprite canvas (comic-strip style) so
    // the bubble overlaps the sprite top rather than the name label above it.
    // LV_ALIGN_TOP_MID anchors inside the sprite canvas bounds, keeping the
    // name label fully visible at all times.
    lv_obj_set_width(speech_bubble_lbl, canvas_w - 16);  // fit within sprite canvas width
    lv_obj_align_to(speech_bubble_lbl, sprite_canvas, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_add_flag(speech_bubble_lbl, LV_OBJ_FLAG_HIDDEN);

    // Seed the first auto-speech attempt for ~60s after the screen is built.
    if (g_speech_next_attempt_ms == 0) {
        g_speech_next_attempt_ms = millis() + 60000;
    }

    // ----- Stats card-button -----
    // Full-width tappable card at the bottom, matching the USAGE/MENU panel
    // idiom. Replaces the prior corner-label affordance (was too small to tap
    // per user feedback).
    {
        const int card_w = caps.width - 2 * SP_M;
        const int card_h = 50;
        const int card_x = SP_M;
        const int card_y = caps.height - card_h - SP_M;

        lv_obj_t* stats_card = make_panel(home_obj, card_x, card_y, card_w, card_h);
        lv_obj_add_flag(stats_card, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* label = lv_label_create(stats_card);
        lv_obj_set_style_text_font(label, theme_font_body(), 0);
        lv_obj_set_style_text_color(label, THEME_TEXT, 0);
        lv_label_set_text(label, "View stats  >");
        lv_obj_center(label);
        // Don't intercept the click — the card is the click target.
        lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_add_event_cb(stats_card, [](lv_event_t* e) {
            (void)e;
            ui_show_screen(SCREEN_STATS);
        }, LV_EVENT_CLICKED, nullptr);
    }

    // ----- Event toast (rendered on root so it's visible from both views) -----
    toast_lbl = lv_label_create(root);
    lv_label_set_text(toast_lbl, "");
    lv_obj_set_style_text_font(toast_lbl, theme_font_display_s(), 0);
    lv_obj_set_style_text_color(toast_lbl, THEME_AMBER, 0);
    lv_obj_set_style_bg_color(toast_lbl, THEME_PANEL, 0);
    lv_obj_set_style_bg_opa(toast_lbl, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(toast_lbl, SP_M, 0);
    lv_obj_set_style_radius(toast_lbl, 8, 0);
    lv_obj_align(toast_lbl, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_add_flag(toast_lbl, LV_OBJ_FLAG_HIDDEN);
}

void pet_screen_init(lv_obj_t* parent) {
    root = parent;
    if (!root) return;
    build_detail_view();
    build_home_view();
}

// Per-stage base cell size. Stage 0 is 60% to read as "juvenile"; all
// others use the layout's base cell. Returned in pixels.
static int stage_cell_size(int base_cell, uint8_t stage) {
    if (stage == 0) return (base_cell * 6) / 10;   // 60% of base
    return base_cell;
}

// Precomputed unit-circle coordinates for the halo overlay (60 samples, one
// per 6°). Scaled by 1000 and stored as int16_t to avoid float math inside
// the per-frame hot path. Initialized lazily on first overlay_halo() call.
static int16_t g_halo_cos_x60[60];
static int16_t g_halo_sin_x60[60];
static bool    g_halo_table_init = false;

static void init_halo_table(void) {
    if (g_halo_table_init) return;
    for (int i = 0; i < 60; i++) {
        float a = (i * 6.0f) * (3.14159265f / 180.0f);  // 6 degrees per step
        g_halo_cos_x60[i] = (int16_t)(cosf(a) * 1000.0f);
        g_halo_sin_x60[i] = (int16_t)(sinf(a) * 1000.0f);
    }
    g_halo_table_init = true;
}

// 2Hz pulsing ring drawn just outside the sprite bounds. Opacity oscillates
// ~40%-80% via a sin-wave on lv_tick_get(). Drawn 2-pixel wide using
// lv_canvas_set_px directly — cheap, no extra widget allocation per frame.
static void overlay_ring(int sub_w_px) {
    if (!sprite_canvas) return;
    uint32_t now = lv_tick_get();
    float t = (float)(now % 500) / 500.0f;                  // 0..1 every 0.5s (2Hz)
    float wave = (sinf(t * 2.0f * 3.14159f) + 1.0f) * 0.5f; // 0..1
    uint8_t alpha = (uint8_t)(102 + wave * 102);            // 102 (~40%) to 204 (~80%)

    // Sprite is centered with 24px padding on each side; ring sits 6px
    // outside the sprite bounds and always has room.
    int ring_radius = sub_w_px / 2 + 6;
    if (ring_radius > g_canvas_w_alloc / 2 - 2)
        ring_radius = g_canvas_w_alloc / 2 - 2;   // belt-and-suspenders guard
    int cx = g_canvas_w_alloc / 2;
    int cy = g_canvas_w_alloc / 2;
    lv_color_t ring_color = THEME_ACCENT;

    // Note: lv_canvas_set_px's `alpha` argument is silently ignored on
    // RGB565 framebuffers; the ring draws at fixed color and the sin-wave
    // alpha computation above is a no-op pending a canvas-format upgrade.

    // Square outline (not a true ring) — matches the pixel-art grid aesthetic
    // and is cheap to draw with axis-aligned set_px loops.

    // Top and bottom edges
    for (int dx = -ring_radius; dx <= ring_radius; dx++) {
        int x = cx + dx;
        int y_top = cy - ring_radius;
        int y_bot = cy + ring_radius;
        if (x < 0 || x >= g_canvas_w_alloc) continue;
        if (y_top >= 0 && y_top < g_canvas_w_alloc - 1) {
            lv_canvas_set_px(sprite_canvas, x, y_top,     ring_color, alpha);
            lv_canvas_set_px(sprite_canvas, x, y_top + 1, ring_color, alpha);
        }
        if (y_bot >= 1 && y_bot < g_canvas_w_alloc) {
            lv_canvas_set_px(sprite_canvas, x, y_bot,     ring_color, alpha);
            lv_canvas_set_px(sprite_canvas, x, y_bot - 1, ring_color, alpha);
        }
    }
    // Left and right edges
    for (int dy = -ring_radius; dy <= ring_radius; dy++) {
        int y = cy + dy;
        int x_l = cx - ring_radius;
        int x_r = cx + ring_radius;
        if (y < 0 || y >= g_canvas_w_alloc) continue;
        if (x_l >= 0 && x_l < g_canvas_w_alloc - 1) {
            lv_canvas_set_px(sprite_canvas, x_l,     y, ring_color, alpha);
            lv_canvas_set_px(sprite_canvas, x_l + 1, y, ring_color, alpha);
        }
        if (x_r >= 1 && x_r < g_canvas_w_alloc) {
            lv_canvas_set_px(sprite_canvas, x_r,     y, ring_color, alpha);
            lv_canvas_set_px(sprite_canvas, x_r - 1, y, ring_color, alpha);
        }
    }
}

// Four corner sparkles, each blinking at ~1Hz with a 0.25s stagger so they
// twinkle out of phase. White pixels at the four corners of the sprite bounds.
static void overlay_sparkles(int sub_w_px) {
    if (!sprite_canvas) return;
    uint32_t now = lv_tick_get();
    int cx = g_canvas_w_alloc / 2;
    int cy = g_canvas_w_alloc / 2;
    // 24px padding outside the sprite gives sparkles room to land in the
    // black border. Clamp remains as a belt-and-suspenders bound.
    int half = sub_w_px / 2 + 10;
    if (half > g_canvas_w_alloc / 2 - 3)
        half = g_canvas_w_alloc / 2 - 3;

    struct { int dx, dy, phase; } corners[4] = {
        { -half, -half, 0    },
        {  half, -half, 250  },
        { -half,  half, 500  },
        {  half,  half, 750  },
    };
    for (int i = 0; i < 4; i++) {
        bool on = ((now + corners[i].phase) / 500) % 2;   // 1Hz on/off
        if (!on) continue;
        int x = cx + corners[i].dx;
        int y = cy + corners[i].dy;
        if (x >= 1 && x < g_canvas_w_alloc - 1 && y >= 1 && y < g_canvas_w_alloc - 1) {
            // 3-pixel "plus" shape so the sparkle reads as a star, not a dot.
            lv_canvas_set_px(sprite_canvas, x,     y,     lv_color_white(), LV_OPA_COVER);
            lv_canvas_set_px(sprite_canvas, x - 1, y,     lv_color_white(), LV_OPA_COVER);
            lv_canvas_set_px(sprite_canvas, x + 1, y,     lv_color_white(), LV_OPA_COVER);
            lv_canvas_set_px(sprite_canvas, x,     y - 1, lv_color_white(), LV_OPA_COVER);
            lv_canvas_set_px(sprite_canvas, x,     y + 1, lv_color_white(), LV_OPA_COVER);
        }
    }
}

// 24-pixel soft halo gradient out from sprite center. Drawn as concentric
// rings with linearly fading alpha. Only used briefly between L30 and graduate.
static void overlay_halo(int sub_w_px) {
    if (!sprite_canvas || !sprite_canvas_buf) return;
    init_halo_table();
    int cx = g_canvas_w_alloc / 2;
    int cy = g_canvas_w_alloc / 2;
    // Keep r_inner just outside the sprite edge so the halo never overdraws
    // the sprite (lv_canvas_set_px alpha is ignored on RGB565, so an inward
    // ring would render as solid amber over the sprite's edge pixels).
    // r_outer is clamped to the canvas instead — at full sprite size the
    // halo gets shorter but stays positioned correctly.
    int r_inner = sub_w_px / 2 + 8;
    int r_outer = r_inner + 24;
    if (r_outer > g_canvas_w_alloc / 2 - 1)
        r_outer = g_canvas_w_alloc / 2 - 1;

    // Note: lv_canvas_set_px's `alpha` argument is silently ignored on
    // RGB565 framebuffers; the halo draws at uniform color and the
    // computed `a` gradient is a no-op pending a canvas-format upgrade.

    // Pre-build the halo color as a raw RGB565 value for direct buffer writes,
    // bypassing LVGL's per-call bounds checking on lv_canvas_set_px.
    // THEME_ACCENT = 0xd97757 → R=0x1B, G=0x3B, B=0x17 in RGB565 components.
    const uint16_t halo_px = (uint16_t)(
        ((0xd9u >> 3) << 11) |   // R5
        ((0x77u >> 2) << 5)  |   // G6
        ((0x57u >> 3))           // B5
    );

    for (int r = r_inner; r < r_outer; r++) {
        // alpha fades from 150 at inner to ~6 at outer (one ring shy of 0).
        // (Alpha is kept for future ARGB8888 canvas upgrade; unused on RGB565.)
        // int a = 150 - ((r - r_inner) * 150) / 24;  // unused on RGB565
        for (int i = 0; i < 60; i++) {
            int x = cx + (r * g_halo_cos_x60[i]) / 1000;
            int y = cy + (r * g_halo_sin_x60[i]) / 1000;
            if (x >= 0 && x < g_canvas_w_alloc && y >= 0 && y < g_canvas_w_alloc) {
                sprite_canvas_buf[y * g_canvas_w_alloc + x] = halo_px;
            }
        }
    }
}

static void render_sprite_frame(void) {
    if (!sprite_canvas_buf || !sprite_canvas) return;
    const Pet* p = pet_current();
    if (!p) return;

    uint8_t stage = pet_evolution_stage(p);
    int cell = stage_cell_size(g_base_cell, stage);
    int sub_w = 20 * cell;

    lv_color_t black = THEME_BG;
    lv_canvas_fill_bg(sprite_canvas, black, LV_OPA_COVER);

    // Determine sad-filter state once before rendering so we can pass it to
    // splash_render_to_buf as the `dim` flag. This moves the per-pixel halving
    // work to the palette level (10 entries) instead of the full sprite buffer
    // (up to 32,400 pixels), cutting the cost by ~3,000x at 4Hz.
    PetStats _stats_for_filter = pet_stats();
    uint8_t  _min_stat_filter = _stats_for_filter.satiety;
    if (_stats_for_filter.spirit < _min_stat_filter) _min_stat_filter = _stats_for_filter.spirit;
    if (_stats_for_filter.bond   < _min_stat_filter) _min_stat_filter = _stats_for_filter.bond;
    bool _dim = (_min_stat_filter < SAD_STAT_THRESHOLD);

    int offset = (g_canvas_w_alloc - sub_w) / 2;
    uint16_t* sub_dest = sprite_canvas_buf + (offset * g_canvas_w_alloc + offset);
    splash_render_to_buf(sub_dest, g_canvas_w_alloc, cell,
        pet_species_animation_index(p->species_id), sprite_frame_idx,
        p->is_shiny != 0, _dim);

    if (stage >= 2)  overlay_ring(sub_w);
    if (stage >= 3)  overlay_sparkles(sub_w);
    if (stage >= 4)  overlay_halo(sub_w);

    lv_obj_invalidate(sprite_canvas);
}

// Stat bands match SAD_STAT_THRESHOLD (=20, in pet.h) so the sprite-sad
// filter and the card-red state stay aligned.
static lv_color_t stat_color(uint8_t v) {
    if (v >= 50) return THEME_GREEN;
    if (v >= SAD_STAT_THRESHOLD) return THEME_AMBER;
    return THEME_RED;
}

// stat_idx: 0=Sat, 1=Spi, 2=Bnd. Returns a const string in BSS.
static const char* stat_band_word(int stat_idx, uint8_t v) {
    static const char* WORDS[3][3] = {
        { "Hungry",  "Peckish",  "Full"  },   // Sat
        { "Bored",   "Restless", "Happy" },   // Spi
        { "Lonely",  "Aloof",    "Loved" },   // Bnd
    };
    int band = (v >= 50) ? 2 : (v >= SAD_STAT_THRESHOLD) ? 1 : 0;
    if (stat_idx < 0 || stat_idx > 2) return "";
    return WORDS[stat_idx][band];
}

static void refresh_status_labels(void) {
    if (!care_bar[0] || !care_bar[1] || !care_bar[2]) return;

    // ----- Phase 3: refresh care widgets (detail view) -----
    PetStats stats = pet_stats();
    uint8_t care_values[3] = { stats.satiety, stats.spirit, stats.bond };
    uint32_t cds[3] = {
        stats.feed_cooldown_remaining,
        stats.play_cooldown_remaining,
        stats.pet_cooldown_remaining,
    };
    for (int i = 0; i < 3; i++) {
        if (care_bar[i]) {
            lv_bar_set_value(care_bar[i], care_values[i], LV_ANIM_ON);
            lv_obj_set_style_bg_color(care_bar[i], stat_color(care_values[i]),
                                      LV_PART_INDICATOR);
        }
        if (care_value_lbl[i]) {
            lv_label_set_text_fmt(care_value_lbl[i], "%u", (unsigned)care_values[i]);
        }
        if (care_subtitle_lbl[i]) {
            // Subtitle = band word + cooldown status. Without the cooldown
            // hint the user has no way to know why a tap got rejected, which
            // is why "refilling isn't working" looks like a bug. Show
            // "ready" when the action is available.
            char buf[40];
            const char* word = stat_band_word(i, care_values[i]);
            if (care_values[i] >= 100) {
                snprintf(buf, sizeof(buf), "%s - full", word);
            } else if (cds[i] > 0) {
                uint32_t m = cds[i] / 60;
                if (m >= 1) snprintf(buf, sizeof(buf), "%s - %lum",  word, (unsigned long)m);
                else        snprintf(buf, sizeof(buf), "%s - %lus",  word, (unsigned long)cds[i]);
            } else {
                snprintf(buf, sizeof(buf), "%s - ready", word);
            }
            lv_label_set_text(care_subtitle_lbl[i], buf);
            lv_obj_set_style_text_color(care_subtitle_lbl[i], stat_color(care_values[i]), 0);
        }
    }

    // Home name label (Task 1.2) — keep in sync with Task 1.3 rename flow
    if (name_lbl) {
        const Pet* p = pet_current();
        lv_label_set_text(name_lbl, (p && p->name[0]) ? p->name : "");
    }

    // Vacation banner (Task 1.5) — show when active, hide when off.
    if (vacation_banner_lbl) {
        if (pet_is_vacation_active()) {
            char buf[32];
            snprintf(buf, sizeof(buf), "Vacation - %ud left",
                     (unsigned)pet_vacation_days_remaining());
            lv_label_set_text(vacation_banner_lbl, buf);
            lv_obj_clear_flag(vacation_banner_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(vacation_banner_lbl, "");
            lv_obj_add_flag(vacation_banner_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Streak row (Task 3.1 / Task 2.7) — composite (max of care, usage). Hide when 0.
    if (streak_row_obj && streak_care_lbl) {
        uint16_t composite_streak = streak_care_days() > streak_usage_days()
                                      ? streak_care_days()
                                      : streak_usage_days();
        if (composite_streak > 0) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%u", (unsigned)composite_streak);
            lv_label_set_text(streak_care_lbl, buf);
            lv_obj_clear_flag(streak_row_obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(streak_row_obj, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Care dot (Task 2.7) — recolor based on worst-of-three stat.
    if (care_dot) {
        PetStats st = pet_stats();
        uint8_t worst = st.satiety;
        if (st.spirit < worst) worst = st.spirit;
        if (st.bond   < worst) worst = st.bond;
        lv_color_t dot_color = (worst > 50) ? THEME_GREEN
                             : (worst > 25) ? THEME_AMBER
                             : THEME_RED;
        lv_obj_set_style_bg_color(care_dot, dot_color, 0);
    }
}

void pet_screen_show(void) {
    uint32_t t_pet_start = millis();
    pet_view = PET_HOME;
    ui_status_bar_set_title("PET");
    ui_status_bar_set_back_cb(nullptr);
    if (home_obj)   lv_obj_clear_flag(home_obj, LV_OBJ_FLAG_HIDDEN);
    if (detail_obj) lv_obj_add_flag(detail_obj, LV_OBJ_FLAG_HIDDEN);
    sprite_frame_idx = 0;
    refresh_status_labels();
    refresh_shiny_sparkle();   // sync overlay to current pet's is_shiny (P1-9)
    render_sprite_frame();
    PetStats _s = pet_stats();
    pet_cursor = (_s.spirit < _s.satiety) ? 1 : 0;
    refresh_cursor_visual();

    // Fire a greeting bubble immediately on screen entry. salt uses millis/60
    // so the greeting line varies across visits without changing mid-view.
    uint32_t salt = (uint32_t)(millis() / 60000);
    show_speech_bubble(pet_speech_pick(SPEECH_GREETING, salt));

    // Push the next auto-speech attempt back so the greeting doesn't get
    // immediately clobbered by the periodic trigger. Re-arm in ~2 minutes.
    g_speech_next_attempt_ms = (uint32_t)millis() + 120000;

    // Daily recap modal (Task 3.2) — show once per day on first pet-screen entry.
    maybe_show_recap();

    Serial.printf("[xtime] pet_screen_show built/refreshed in %lums\n",
                  (unsigned long)(millis() - t_pet_start));
}

static void enter_detail_view(void) {
    pet_view = PET_CARE_DETAIL;
    ui_status_bar_set_title("< PET");
    ui_status_bar_set_back_cb(exit_detail_view);
    if (home_obj)   lv_obj_add_flag(home_obj,   LV_OBJ_FLAG_HIDDEN);
    if (detail_obj) lv_obj_clear_flag(detail_obj, LV_OBJ_FLAG_HIDDEN);
    refresh_status_labels();
    refresh_cursor_visual();
    // Reset indev press state so the next tap-down lands cleanly on the
    // detail view's cards, not on the home-view widget that triggered us.
    lv_indev_t* indev = lv_indev_active();
    if (indev) lv_indev_wait_release(indev);
}

static void exit_detail_view(void) {
    pet_view = PET_HOME;
    ui_status_bar_set_title("PET");
    ui_status_bar_set_back_cb(nullptr);
    if (detail_obj) lv_obj_add_flag(detail_obj, LV_OBJ_FLAG_HIDDEN);
    if (home_obj)   lv_obj_clear_flag(home_obj, LV_OBJ_FLAG_HIDDEN);
    refresh_status_labels();
}


void pet_screen_tick(void) {
    if (!home_obj) return;   // init failed; nothing to tick
    uint32_t now = lv_tick_get();

    if (now - sprite_last_frame_ms >= SPRITE_FRAME_MS) {
        sprite_last_frame_ms = now;
        sprite_frame_idx++;
        render_sprite_frame();
    }

    if (now - labels_last_refresh_ms >= LABEL_REFRESH_MS) {
        labels_last_refresh_ms = now;
        refresh_status_labels();
    }

    // ----- Phase 3 → 4: floating "+25" label fade-out -----
    for (int i = 0; i < 3; i++) {
        if (care_float_start_ms[i] == 0) continue;
        uint32_t elapsed = now - care_float_start_ms[i];
        if (elapsed >= CARE_FLOAT_MS) {
            care_float_start_ms[i] = 0;
            if (care_float_lbl[i]) lv_obj_add_flag(care_float_lbl[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        // Drift upward over CARE_FLOAT_MS
        if (!care_float_lbl[i]) continue;
        int drift = -(int)((elapsed * 24) / CARE_FLOAT_MS);
        if (i < 2 && care_row[i]) {
            // Sat/Spi: anchor right of the value label, drift upward.
            lv_obj_align_to(care_float_lbl[i], care_value_lbl[i],
                            LV_ALIGN_OUT_RIGHT_TOP, SP_S, drift);
        } else if (i == 2 && sprite_canvas) {
            // Bnd: relative to sprite — unchanged.
            lv_obj_align_to(care_float_lbl[i], sprite_canvas,
                LV_ALIGN_OUT_TOP_MID, 0, -2 + drift);
        }
    }

    // Toast decay (wraparound-safe via unsigned subtraction)
    if (toast_start_ms != 0 && (now - toast_start_ms) >= TOAST_MS) {
        toast_start_ms = 0;
        if (toast_lbl) lv_obj_add_flag(toast_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    // ----- Speech bubble auto-dismiss -----
    if (speech_bubble_show_until_ms != 0 &&
        (uint32_t)millis() >= speech_bubble_show_until_ms) {
        hide_speech_bubble();
    }

    // ----- Periodic speech trigger (home view only, 5-15 min cadence) -----
    // Only fires when the pet home view is the active sub-view — we don't
    // show bubbles on the care-detail panel or on hidden screens.
    if (pet_view == PET_HOME &&
        g_speech_next_attempt_ms != 0 &&
        (uint32_t)millis() >= g_speech_next_attempt_ms) {

        speech_category_t cat = pet_speech_current_category();
        if (cat != SPEECH_NONE) {
            // salt = seconds / 60 → changes every minute so lines vary over
            // the day without churning within a single display window.
            uint32_t salt = (uint32_t)(millis() / 60000);
            show_speech_bubble(pet_speech_pick(cat, salt));
        }
        // Schedule next attempt in 5–15 minutes (300–900 s). Use millis() %
        // 601 for a cheap pseudo-random spread without needing stdlib rand.
        uint32_t spread_ms = (300 + (uint32_t)(millis() % 601)) * 1000;
        g_speech_next_attempt_ms = (uint32_t)millis() + spread_ms;
    }
}

bool pet_screen_on_button(ui_btn_t btn) {
    // Recap modal up: any physical button dismisses it and is consumed,
    // so MID can't cycle out from under the modal (P0-2) and so
    // touch-less users on AMOLED-1.8 have a working dismiss path (P1-18).
    if (recap_modal_obj) {
        lv_obj_delete(recap_modal_obj);
        recap_modal_obj = nullptr;
        recap_mark_shown();
        Serial.printf("[recap] modal dismissed by button=%d\n", (int)btn);
        return true;
    }
    if (pet_view == PET_HOME) {
        switch (btn) {
            case UI_BTN_LEFT:
                return true;                   // only one focus target on home — no-op
            case UI_BTN_RIGHT:
                enter_detail_view();
                return true;
            case UI_BTN_MID:
                return false;                  // let global cycle to MENU
        }
        return false;
    }
    // PET_CARE_DETAIL
    switch (btn) {
        case UI_BTN_LEFT:
            pet_cursor = (pet_cursor + 1) % 2;
            refresh_cursor_visual();
            return true;
        case UI_BTN_RIGHT: {
            bool ok = false;
            if (pet_cursor == 0) {
                ok = pet_feed();
                Serial.printf("pet_screen: R→Feed → %s\n", ok ? "OK" : "REJECT");
            } else {
                ok = pet_play();
                Serial.printf("pet_screen: R→Play → %s\n", ok ? "OK" : "REJECT");
            }
            if (ok) {
                refresh_status_labels();
                care_show_float(pet_cursor);
            }
            return true;
        }
        case UI_BTN_MID:
            exit_detail_view();
            return true;                       // consume — do NOT cycle screens
    }
    return false;
}

// Public entrypoint used by the `speech` serial cmd and future triggers.
void pet_screen_force_speech_bubble(const char* text) {
    show_speech_bubble(text);
}

// Force-show the recap modal for testing. Uses yesterday's snapshot if
// available; falls back to a nullptr snap (shows empty stats). Does NOT
// call recap_mark_shown() so it can be triggered multiple times.
void pet_screen_force_recap(void) {
    const DailySnapshot* snap = recap_yesterday();
    show_recap_modal(snap);
}

void pet_screen_hide_modals(void) {
    if (!recap_modal_obj) return;
    lv_obj_delete(recap_modal_obj);
    recap_modal_obj = nullptr;
    Serial.println("[recap] modal torn down on screen switch");
}

void pet_screen_flash_toast(const char* text) {
    if (!text) return;
    flash_toast(text);
}

void pet_screen_on_event(pet_event_t ev) {
    if (ev == PET_EVENT_HATCHED) {
        // Show the naming keyboard regardless of which screen is currently
        // active — a graduation can happen while the user is on a different
        // screen. Navigate to SCREEN_PET first so the keyboard makes sense
        // contextually, then show the overlay.
        // Suppress the recap that pet_screen_show() would trigger via
        // maybe_show_recap() — stacking both modals is jarring. The recap
        // fires normally on the next session instead.
        g_suppress_next_recap = true;
        ui_show_screen(SCREEN_PET);
        // After ui_show_screen runs pet_screen_show, the sparkle overlay
        // is in sync — but if the hatch fired while already on SCREEN_PET
        // (ui_show_screen would no-op), re-sync explicitly (P1-9).
        refresh_shiny_sparkle();
        const Pet* p = pet_current();
        kbd_overlay_show("Name your pet",
                         (p && p->name[0]) ? p->name : "",
                         15,
                         on_pet_name_chosen,
                         nullptr);
        return;
    }

    if (ui_get_current_screen() != SCREEN_PET) return;   // silent if not viewing
    if (!toast_lbl) return;
    const char* txt = toast_text_for(ev);
    if (!txt || !txt[0]) return;
    toast_start_ms = lv_tick_get();
    lv_label_set_text(toast_lbl, txt);
    lv_obj_clear_flag(toast_lbl, LV_OBJ_FLAG_HIDDEN);
}
