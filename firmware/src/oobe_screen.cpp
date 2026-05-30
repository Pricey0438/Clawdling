#include "oobe_screen.h"
#include "theme.h"
#include "hal/board_caps.h"
#include "ui.h"
#include "qrcode.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

// Update this URL when docs move; recompile.
#define OOBE_QR_URL "https://github.com/Pricey0438/Clawdling#quickstart"

static lv_obj_t* s_root      = nullptr;
static lv_obj_t* s_canvas    = nullptr;
static uint16_t* s_qr_buf    = nullptr;
static lv_obj_t* s_url_lbl   = nullptr;
static lv_obj_t* s_head_lbl  = nullptr;

void oobe_screen_init(lv_obj_t* parent) {
    if (s_root) return;

    const BoardCaps& caps = board_caps();

    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, caps.width, caps.height);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, THEME_BG, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

    int sbh = theme_status_bar_h();

    // Headline
    s_head_lbl = lv_label_create(s_root);
    lv_label_set_text(s_head_lbl, "Hi! Set me up.");
    lv_obj_set_style_text_font(s_head_lbl, theme_font_display_s(), 0);
    lv_obj_set_style_text_color(s_head_lbl, THEME_TEXT, 0);
    lv_obj_align(s_head_lbl, LV_ALIGN_TOP_MID, 0, sbh + SP_M);

    // Encode the QR. URL is ~60 chars; QR version 4 (33×33) at ECC_LOW
    // fits up to 80 alphanumeric chars, comfortable headroom.
    QRCode qr;
    uint8_t qr_buf[400];   // version 4 needs ~200 bytes; 400 is safe
    qrcode_initText(&qr, qr_buf, 4, ECC_LOW, OOBE_QR_URL);

    // Vertical budget: status bar + SP_M gap + headline (display_s ~34px)
    // + SP_M gap above QR, then SP_M gap + URL label (~22px) + SP_M margin
    // below. The QR is square so the horizontal budget (caps.width minus
    // small side margins) also bounds it.
    const int HEADLINE_H = 36;
    const int URL_H      = 24;
    const int qr_top     = sbh + SP_M + HEADLINE_H + SP_M;
    const int qr_bottom  = caps.height - (URL_H + SP_M + SP_M);
    const int v_budget   = qr_bottom - qr_top;
    const int h_budget   = caps.width - 2 * SP_M;
    const int budget     = (v_budget < h_budget) ? v_budget : h_budget;
    int cell = budget / (int)qr.size;
    if (cell < 2) cell = 2;
    int px = qr.size * cell;

    size_t buf_bytes = (size_t)px * px * sizeof(uint16_t);
    s_qr_buf = (uint16_t*)heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    if (!s_qr_buf) {
        Serial.printf("[oobe] QR canvas alloc failed (%u bytes)\n", (unsigned)buf_bytes);
        s_canvas = nullptr;
    } else {
        const uint16_t WHITE_565 = 0xFFFF;
        const uint16_t BLACK_565 = 0x0000;
        for (int i = 0; i < px * px; i++) s_qr_buf[i] = WHITE_565;
        for (uint8_t gy = 0; gy < qr.size; gy++) {
            for (uint8_t gx = 0; gx < qr.size; gx++) {
                if (qrcode_getModule(&qr, gx, gy)) {
                    for (int dy = 0; dy < cell; dy++) {
                        uint16_t* row = &s_qr_buf[(gy * cell + dy) * px + gx * cell];
                        for (int dx = 0; dx < cell; dx++) row[dx] = BLACK_565;
                    }
                }
            }
        }

        s_canvas = lv_canvas_create(s_root);
        if (s_canvas) {
            lv_canvas_set_buffer(s_canvas, s_qr_buf, px, px, LV_COLOR_FORMAT_RGB565);
            // Anchor the QR top below the headline + gap so it never collides
            // with the headline text on any font/board combination.
            const int qr_x = (caps.width - px) / 2;
            lv_obj_set_pos(s_canvas, qr_x, qr_top);
        } else {
            // Canvas creation failed under PSRAM pressure. s_qr_buf is
            // intentionally leaked: this screen has no teardown path
            // (init guard tripped via s_root) and a retry would just
            // reallocate.
            Serial.println("[oobe] lv_canvas_create failed");
        }
    }

    // URL label below the QR.
    s_url_lbl = lv_label_create(s_root);
    lv_label_set_text(s_url_lbl, OOBE_QR_URL);
    lv_obj_set_style_text_font(s_url_lbl, theme_font_label(), 0);
    lv_obj_set_style_text_color(s_url_lbl, THEME_DIM, 0);
    lv_obj_align(s_url_lbl, LV_ALIGN_BOTTOM_MID, 0, -SP_M);

    Serial.printf("[oobe] init done (qr_size=%u, cell=%d, px=%d)\n",
                  (unsigned)qr.size, cell, px);
}

void oobe_screen_show(void) {
    if (!s_root) return;
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    ui_status_bar_set_title("SETUP");
}

void oobe_screen_hide(void) {
    if (!s_root) return;
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}
