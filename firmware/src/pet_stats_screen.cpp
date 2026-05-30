#include "pet_stats_screen.h"
#include "theme.h"
#include "hal/board_caps.h"
#include "ui.h"
#include "ui_panels.h"
#include "pet.h"
#include "streak.h"
#include "achievements.h"
#include "recap.h"
#include <Arduino.h>

// ── Layout overview ──────────────────────────────────────────────────
// USAGE-inspired stack of big cards. Each care stat gets its own card
// with a Styrene 48 value as the hero number and the label as a small
// pill on the right (matches USAGE's "Current"/"Weekly" pill idiom).
//
// Satiety + Spirit cards are tappable: tap when ready commits the
// refill (pet_feed / pet_play). Bond is display-only with a "tap pet"
// hint — bond is committed via sprite tap on SCREEN_PET, not here.
//
// Footer: slim single-line "Lv N · XP m/n · streak Xd" tucked at the
// bottom. The earlier dense 7-day grid + quest + recap snippets are
// gone — they didn't pull weight at the new breathing-room scale.

static lv_obj_t* s_root         = nullptr;
static lv_obj_t* s_sat_card     = nullptr;
static lv_obj_t* s_spi_card     = nullptr;
static lv_obj_t* s_bnd_card     = nullptr;
static lv_obj_t* s_footer_card  = nullptr;

// Per-card widgets (3 stats: 0=Sat, 1=Spi, 2=Bnd)
static lv_obj_t* s_stat_val_lbl[3]   = {nullptr, nullptr, nullptr};
static lv_obj_t* s_stat_pill_lbl[3]  = {nullptr, nullptr, nullptr};
static lv_obj_t* s_stat_bar[3]       = {nullptr, nullptr, nullptr};
static lv_obj_t* s_stat_hint_lbl[3]  = {nullptr, nullptr, nullptr};

// Footer widgets
static lv_obj_t* s_footer_lvl_lbl    = nullptr;
static lv_obj_t* s_footer_xp_lbl     = nullptr;
static lv_obj_t* s_footer_streak_lbl = nullptr;

// Toast that briefly flashes the card border on a successful refill tap.
// Implemented via a one-shot bar tint change on next refresh.
static uint32_t s_flash_until_ms[3] = {0, 0, 0};

static void stats_back_to_pet(void) {
    ui_show_screen(SCREEN_PET);
}

// Shared tap-to-refill body — used by both the LVGL click callback and
// the public sim entrypoint so test-injection runs the exact same path.
static void handle_stat_tap(int idx) {
    if (idx < 0 || idx > 1) return;   // Bond is read-only here
    PetStats st = pet_stats();
    uint32_t cd = (idx == 0) ? st.feed_cooldown_remaining
                              : st.play_cooldown_remaining;
    if (cd > 0) {
        Serial.printf("[stats] tap stat=%d rejected — cd=%lus\n", idx, (unsigned long)cd);
        return;
    }
    bool ok = (idx == 0) ? pet_feed() : pet_play();
    Serial.printf("[stats] tap stat=%d → %s\n", idx, ok ? "OK" : "REJECT");
    if (ok) {
        s_flash_until_ms[idx] = millis() + 800;
        pet_stats_screen_show();   // re-read values + repaint
    }
}

static void stat_card_tap_cb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    handle_stat_tap(idx);
}

void pet_stats_screen_sim_tap(int idx) {
    handle_stat_tap(idx);
}

void pet_stats_screen_init(lv_obj_t* parent) {
    if (s_root) return;
    const BoardCaps& caps = board_caps();
    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, caps.width, caps.height);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, THEME_BG, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    const int sbh        = theme_status_bar_h();
    const int panel_x    = SP_M;
    const int panel_w    = caps.width - 2 * SP_M;
    const int gap        = SP_S;
    const int card_h     = 100;
    const int footer_h   = 56;
    // Computed top y after the status bar + a small top gap.
    int y = sbh + gap;

    // ── 3 big stat cards ──────────────────────────────────────────────
    const char* labels[3]   = {"Satiety", "Spirit", "Bond"};
    const char* hints_init[3] = {"ready", "ready", "tap pet to bond"};
    lv_obj_t** cards[3]     = {&s_sat_card, &s_spi_card, &s_bnd_card};

    for (int i = 0; i < 3; i++) {
        lv_obj_t* card = make_panel(s_root, panel_x, y, panel_w, card_h);
        *cards[i] = card;
        y += card_h + gap;

        // Tap handler for Sat (0) + Spi (1) only. Bond (2) is read-only.
        if (i < 2) {
            lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(card, stat_card_tap_cb, LV_EVENT_CLICKED,
                                (void*)(intptr_t)i);
        }

        // Big hero value — left-aligned, Tiempos display font (huge).
        s_stat_val_lbl[i] = lv_label_create(card);
        lv_obj_set_style_text_font(s_stat_val_lbl[i], theme_font_display_s(), 0);
        lv_obj_set_style_text_color(s_stat_val_lbl[i], THEME_TEXT, 0);
        lv_label_set_text(s_stat_val_lbl[i], "0");
        lv_obj_align(s_stat_val_lbl[i], LV_ALIGN_LEFT_MID, 0, -10);

        // Label pill — right-aligned, body font.
        s_stat_pill_lbl[i] = lv_label_create(card);
        lv_obj_set_style_text_font(s_stat_pill_lbl[i], theme_font_body(), 0);
        lv_obj_set_style_text_color(s_stat_pill_lbl[i], THEME_TEXT, 0);
        lv_obj_set_style_bg_color(s_stat_pill_lbl[i], THEME_BAR_BG, 0);
        lv_obj_set_style_bg_opa(s_stat_pill_lbl[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_stat_pill_lbl[i], 14, 0);
        lv_obj_set_style_pad_left(s_stat_pill_lbl[i], 10, 0);
        lv_obj_set_style_pad_right(s_stat_pill_lbl[i], 10, 0);
        lv_obj_set_style_pad_top(s_stat_pill_lbl[i], 4, 0);
        lv_obj_set_style_pad_bottom(s_stat_pill_lbl[i], 4, 0);
        lv_label_set_text(s_stat_pill_lbl[i], labels[i]);
        lv_obj_align(s_stat_pill_lbl[i], LV_ALIGN_TOP_RIGHT, 0, 0);
        // Pill is decorative — don't intercept taps on the card.
        lv_obj_clear_flag(s_stat_pill_lbl[i], LV_OBJ_FLAG_CLICKABLE);

        // Progress bar — subtle, full width inside the card.
        s_stat_bar[i] = lv_bar_create(card);
        lv_obj_set_size(s_stat_bar[i], panel_w - 2 * SP_L, 6);
        lv_obj_align(s_stat_bar[i], LV_ALIGN_BOTTOM_LEFT, 0, -22);
        lv_obj_set_style_bg_color(s_stat_bar[i], THEME_BAR_BG, 0);
        lv_obj_set_style_bg_color(s_stat_bar[i], THEME_ACCENT, LV_PART_INDICATOR);
        lv_bar_set_range(s_stat_bar[i], 0, 100);
        lv_bar_set_value(s_stat_bar[i], 0, LV_ANIM_OFF);
        lv_obj_clear_flag(s_stat_bar[i], LV_OBJ_FLAG_CLICKABLE);

        // Hint text — "ready" / "cd 12m" / "tap pet to bond"
        s_stat_hint_lbl[i] = lv_label_create(card);
        lv_obj_set_style_text_font(s_stat_hint_lbl[i], theme_font_label(), 0);
        lv_obj_set_style_text_color(s_stat_hint_lbl[i], THEME_DIM, 0);
        lv_label_set_text(s_stat_hint_lbl[i], hints_init[i]);
        lv_obj_align(s_stat_hint_lbl[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_clear_flag(s_stat_hint_lbl[i], LV_OBJ_FLAG_CLICKABLE);
    }

    // ── Slim footer: Lv · XP · streak ──────────────────────────────────
    s_footer_card = make_panel(s_root, panel_x, y, panel_w, footer_h);

    s_footer_lvl_lbl = lv_label_create(s_footer_card);
    lv_obj_set_style_text_font(s_footer_lvl_lbl, theme_font_body(), 0);
    lv_obj_set_style_text_color(s_footer_lvl_lbl, THEME_TEXT, 0);
    lv_label_set_text(s_footer_lvl_lbl, "Lv 1");
    lv_obj_align(s_footer_lvl_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    s_footer_xp_lbl = lv_label_create(s_footer_card);
    lv_obj_set_style_text_font(s_footer_xp_lbl, theme_font_label(), 0);
    lv_obj_set_style_text_color(s_footer_xp_lbl, THEME_DIM, 0);
    lv_label_set_text(s_footer_xp_lbl, "XP 0 / 100");
    lv_obj_align(s_footer_xp_lbl, LV_ALIGN_CENTER, 0, 0);

    s_footer_streak_lbl = lv_label_create(s_footer_card);
    lv_obj_set_style_text_font(s_footer_streak_lbl, theme_font_body(), 0);
    lv_obj_set_style_text_color(s_footer_streak_lbl, THEME_AMBER, 0);
    lv_label_set_text(s_footer_streak_lbl, "0d");
    lv_obj_align(s_footer_streak_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

    Serial.println("[stats] screen built — 3 big stat cards (USAGE-style) + slim footer");
}

void pet_stats_screen_show(void) {
    if (!s_root) return;
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_HIDDEN);

    const Pet* p = pet_current();
    char buf[48];

    ui_status_bar_set_title("< STATS");
    ui_status_bar_set_back_cb(stats_back_to_pet);

    // ── Care stats: value + bar + hint, per row ──────────────────────
    PetStats st = pet_stats();
    const uint8_t  values[3] = {st.satiety, st.spirit, st.bond};
    const uint32_t cds[3]    = {st.feed_cooldown_remaining,
                                st.play_cooldown_remaining,
                                st.pet_cooldown_remaining};
    const uint32_t now_ms = millis();
    for (int i = 0; i < 3; i++) {
        snprintf(buf, sizeof(buf), "%u", (unsigned)values[i]);
        lv_label_set_text(s_stat_val_lbl[i], buf);
        lv_bar_set_value(s_stat_bar[i], values[i], LV_ANIM_OFF);

        // Bond (i=2) always shows "tap pet to bond" — no cooldown UI since
        // committing happens on the pet screen, not here.
        if (i == 2) {
            lv_label_set_text(s_stat_hint_lbl[i], "tap pet to bond");
            lv_obj_set_style_text_color(s_stat_hint_lbl[i], THEME_DIM, 0);
            continue;
        }

        // Sat / Spi — show "tap to refill" when ready, "cd Xm" on cooldown.
        if (cds[i] == 0) {
            lv_label_set_text(s_stat_hint_lbl[i], "tap to refill");
            lv_obj_set_style_text_color(s_stat_hint_lbl[i], THEME_GREEN, 0);
        } else {
            uint32_t mins = (cds[i] + 59) / 60;
            snprintf(buf, sizeof(buf), "ready in %lum", (unsigned long)mins);
            lv_label_set_text(s_stat_hint_lbl[i], buf);
            lv_obj_set_style_text_color(s_stat_hint_lbl[i], THEME_DIM, 0);
        }

        // Brief green-bar flash on a successful refill tap (set by handler).
        if (s_flash_until_ms[i] > now_ms) {
            lv_obj_set_style_bg_color(s_stat_bar[i], THEME_GREEN, LV_PART_INDICATOR);
        } else {
            lv_obj_set_style_bg_color(s_stat_bar[i], THEME_ACCENT, LV_PART_INDICATOR);
        }
    }

    // ── Footer: Lv · XP · streak ────────────────────────────────────
    snprintf(buf, sizeof(buf), "Lv %u", p ? (unsigned)p->level : 0u);
    lv_label_set_text(s_footer_lvl_lbl, buf);

    uint32_t xp_cur  = p ? p->xp : 0;
    uint32_t xp_next = pet_xp_to_next(p ? p->level : 1);
    snprintf(buf, sizeof(buf), "XP %lu / %lu",
             (unsigned long)xp_cur, (unsigned long)xp_next);
    lv_label_set_text(s_footer_xp_lbl, buf);

    uint16_t care_days = streak_care_days();
    uint16_t use_days  = streak_usage_days();
    uint16_t composite = (care_days > use_days) ? care_days : use_days;
    snprintf(buf, sizeof(buf), "%ud", (unsigned)composite);
    lv_label_set_text(s_footer_streak_lbl, buf);
}

void pet_stats_screen_hide(void) {
    if (!s_root) return;
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    ui_status_bar_set_back_cb(nullptr);
}

bool pet_stats_screen_on_button(ui_btn_t btn) {
    if (btn == UI_BTN_LEFT) {
        ui_show_screen(SCREEN_PET);
        return true;
    }
    return false;
}
