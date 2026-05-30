#include "keyboard_overlay.h"
#include "theme.h"
#include "hal/board_caps.h"
#include <lvgl.h>
#include <Arduino.h>

// ------------------------------------------------------------------
// Module state
// ------------------------------------------------------------------
static lv_obj_t*      s_overlay  = nullptr;   // full-screen modal bg
static lv_obj_t*      s_ta       = nullptr;   // textarea
static lv_obj_t*      s_kbd      = nullptr;   // lv_keyboard
static kbd_done_cb_t  s_cb       = nullptr;
static void*          s_userdata = nullptr;
static uint8_t        s_max_len  = 15;

// ------------------------------------------------------------------
// Internal helpers
// ------------------------------------------------------------------

// Called when the keyboard emits LV_EVENT_READY (OK key) on the textarea.
// Fires on_done with the current textarea text and hides the overlay.
static void on_ta_ready(lv_event_t* e) {
    (void)e;
    if (!s_ta || !s_cb) {
        kbd_overlay_hide();
        return;
    }
    // Copy text out of the textarea's internal buffer BEFORE kbd_overlay_hide()
    // calls lv_obj_delete(s_overlay), which frees s_ta and its internal text
    // buffer. Passing the freed pointer to cb() was a latent UAF.
    char name[256];
    const char* txt = lv_textarea_get_text(s_ta);
    if (txt) {
        strncpy(name, txt, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
    } else {
        name[0] = '\0';
    }
    kbd_done_cb_t cb = s_cb;
    void* ud        = s_userdata;
    kbd_overlay_hide();
    cb(name, ud);
}

// Called when the keyboard emits LV_EVENT_CANCEL (Back/Close key).
// Hides the overlay without calling on_done.
static void on_ta_cancel(lv_event_t* e) {
    (void)e;
    kbd_overlay_hide();
}

// Clamp textarea length to s_max_len on every value-change event.
static void on_ta_value_changed(lv_event_t* e) {
    (void)e;
    if (!s_ta) return;
    static bool re_entrant = false;          // H2 guard
    if (re_entrant) return;
    const char* txt = lv_textarea_get_text(s_ta);
    if (!txt) return;
    size_t len = strlen(txt);
    if (len <= s_max_len) return;
    // Over max: trim safely. Use a heap-allocated null-terminated copy so we
    // don't depend on any stack-buffer size assumption. (H1 fix.)
    char* tmp = (char*)malloc(s_max_len + 1);
    if (!tmp) return;
    size_t copy = s_max_len;
    // Trim on a UTF-8 character boundary (H4 fix): walk back from the cut
    // point until we land on a leading byte (0xxxxxxx or 11xxxxxx).
    while (copy > 0) {
        unsigned char b = (unsigned char)txt[copy];
        if (b < 0x80 || (b & 0xC0) == 0xC0) break;
        copy--;
    }
    memcpy(tmp, txt, copy);
    tmp[copy] = '\0';
    re_entrant = true;
    lv_textarea_set_text(s_ta, tmp);          // may re-emit VALUE_CHANGED — guarded
    re_entrant = false;
    free(tmp);
}

// ------------------------------------------------------------------
// Smartwatch-tuned key maps (waveshare_amoled_216 focus).
// Bezel-less letters; only the OK key carries CHECKED so the CHECKED-state
// style can make it (and only it) terra-cotta. shift/123/backspace are
// control buttons (CLICK_TRIG | NO_REPEAT) but NOT checked, so they render
// as bezel-less labels like the letters. Mode-switch tokens "ABC"/"abc"/"1#"
// are interpreted by lv_keyboard's default event handler.
// ------------------------------------------------------------------
// lv_buttonmatrix_ctrl_t is a strict C++ enum, so width literals and flag
// combos must be cast. B() wraps each entry; KB_FN/KB_OK are the flag bundles.
#define B(x)   ((lv_buttonmatrix_ctrl_t)(x))
#define KB_FN  (LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG)
#define KB_OK  (LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_CHECKED)

static const char* const KB_MAP_LOWER[] = {
    "q","w","e","r","t","y","u","i","o","p","\n",
    "a","s","d","f","g","h","j","k","l","\n",
    "ABC","z","x","c","v","b","n","m",LV_SYMBOL_BACKSPACE,"\n",
    "1#"," ",LV_SYMBOL_OK,""
};
static const char* const KB_MAP_UPPER[] = {
    "Q","W","E","R","T","Y","U","I","O","P","\n",
    "A","S","D","F","G","H","J","K","L","\n",
    "abc","Z","X","C","V","B","N","M",LV_SYMBOL_BACKSPACE,"\n",
    "1#"," ",LV_SYMBOL_OK,""
};
static const char* const KB_MAP_SPECIAL[] = {
    "1","2","3","4","5","6","7","8","9","0","\n",
    "-","_",".","'","!","?","@","#","&","\n",
    "+","=","/",":",";","(",")","\"",LV_SYMBOL_BACKSPACE,"\n",
    "abc"," ",LV_SYMBOL_OK,""
};
// Shared by lower & upper (same shape). 31 entries = 10 + 9 + 9 + 3.
static const lv_buttonmatrix_ctrl_t KB_CTRL_LETTERS[] = {
    B(2),B(2),B(2),B(2),B(2),B(2),B(2),B(2),B(2),B(2),
    B(2),B(2),B(2),B(2),B(2),B(2),B(2),B(2),B(2),
    B(KB_FN | 3), B(2),B(2),B(2),B(2),B(2),B(2),B(2), B(KB_FN | 3),
    B(KB_FN | 3), B(8), B(KB_OK | 3)
};
// Special page. 31 entries = 10 + 9 + 9 + 3.
static const lv_buttonmatrix_ctrl_t KB_CTRL_SPECIAL[] = {
    B(2),B(2),B(2),B(2),B(2),B(2),B(2),B(2),B(2),B(2),
    B(2),B(2),B(2),B(2),B(2),B(2),B(2),B(2),B(2),
    B(2),B(2),B(2),B(2),B(2),B(2),B(2),B(2), B(KB_FN | 3),
    B(KB_FN | 3), B(8), B(KB_OK | 3)
};

// ------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------

void kbd_overlay_show(const char* title,
                      const char* initial_text,
                      uint8_t     max_len,
                      kbd_done_cb_t on_done,
                      void*       user_data)
{
    // Tear down any existing overlay before building a new one.
    kbd_overlay_hide();

    s_cb       = on_done;
    s_userdata = user_data;
    s_max_len  = (max_len == 0) ? 15 : max_len;

    const BoardCaps& caps = board_caps();
    bool large = (caps.height >= 460);

    // ------------------------------------------------------------------
    // Full-screen modal background. Parented to the LVGL active screen so
    // it sits on top of all existing objects. LV_OBJ_FLAG_CLICKABLE
    // ensures touches on the dark area don't fall through to widgets below.
    // ------------------------------------------------------------------
    s_overlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, caps.width, caps.height);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, THEME_BG, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // Bring to front so nothing renders over it.
    lv_obj_move_foreground(s_overlay);

    int sbh     = theme_status_bar_h();
    int y       = sbh + SP_M;

    // ------------------------------------------------------------------
    // Title label
    // ------------------------------------------------------------------
    lv_obj_t* title_lbl = lv_label_create(s_overlay);
    lv_label_set_text(title_lbl, title ? title : "");
    lv_obj_set_style_text_font(title_lbl, theme_font_display_s(), 0);
    lv_obj_set_style_text_color(title_lbl, THEME_TEXT, 0);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, y);
    y += (large ? 40 : 32) + SP_M;

    // ------------------------------------------------------------------
    // Textarea
    // ------------------------------------------------------------------
    int ta_h = large ? 64 : 52;
    s_ta = lv_textarea_create(s_overlay);
    lv_textarea_set_one_line(s_ta, true);
    lv_textarea_set_max_length(s_ta, s_max_len);
    lv_textarea_set_text(s_ta, (initial_text && initial_text[0]) ? initial_text : "");
    lv_textarea_set_placeholder_text(s_ta, "Enter name...");
    lv_obj_set_size(s_ta, caps.width - 40, ta_h);
    lv_obj_align(s_ta, LV_ALIGN_TOP_MID, 0, y);

    // Textarea styling — matches the project's dark aesthetic.
    lv_obj_set_style_bg_color(s_ta, THEME_PANEL, 0);
    lv_obj_set_style_bg_opa(s_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_ta, THEME_ACCENT, 0);
    lv_obj_set_style_border_width(s_ta, 2, 0);
    lv_obj_set_style_radius(s_ta, 8, 0);
    lv_obj_set_style_text_color(s_ta, THEME_TEXT, 0);
    lv_obj_set_style_text_font(s_ta, theme_font_body(), 0);
    lv_obj_set_style_pad_left(s_ta, SP_M, 0);
    lv_obj_set_style_pad_right(s_ta, SP_M, 0);

    // Wire textarea events.
    lv_obj_add_event_cb(s_ta, on_ta_ready,         LV_EVENT_READY,        nullptr);
    lv_obj_add_event_cb(s_ta, on_ta_cancel,        LV_EVENT_CANCEL,       nullptr);
    lv_obj_add_event_cb(s_ta, on_ta_value_changed, LV_EVENT_VALUE_CHANGED, nullptr);

    // ------------------------------------------------------------------
    // LVGL keyboard — docked to the bottom of the overlay.
    // ------------------------------------------------------------------
    s_kbd = lv_keyboard_create(s_overlay);
    lv_keyboard_set_textarea(s_kbd, s_ta);

    // Custom smartwatch maps (declutters the default punctuation/arrow rows).
    lv_keyboard_set_map(s_kbd, LV_KEYBOARD_MODE_TEXT_LOWER, KB_MAP_LOWER,   KB_CTRL_LETTERS);
    lv_keyboard_set_map(s_kbd, LV_KEYBOARD_MODE_TEXT_UPPER, KB_MAP_UPPER,   KB_CTRL_LETTERS);
    lv_keyboard_set_map(s_kbd, LV_KEYBOARD_MODE_SPECIAL,    KB_MAP_SPECIAL, KB_CTRL_SPECIAL);
    lv_keyboard_set_mode(s_kbd, LV_KEYBOARD_MODE_TEXT_LOWER);

    // Size: full width, ~62% of height → ~70px rows, ~48px keys (big targets).
    int kbd_h = (caps.height * 62) / 100;
    lv_obj_set_size(s_kbd, caps.width, kbd_h);
    lv_obj_align(s_kbd, LV_ALIGN_BOTTOM_MID, 0, 0);

    // Keyboard panel background + inter-key spacing. The gap is what makes
    // each key's boundary visible — important because, with no on-device word
    // prediction, users must aim at a clearly-bounded target (fully bezel-less
    // keys read as one slab and taps land on the wrong neighbor).
    lv_obj_set_style_bg_color(s_kbd, THEME_BG, 0);
    lv_obj_set_style_bg_opa(s_kbd, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_kbd, 0, 0);
    lv_obj_set_style_pad_all(s_kbd, SP_S, 0);
    lv_obj_set_style_pad_gap(s_kbd, SP_S, 0);

    // Visible key cells: filled panel-grey with a clear radius, so each key is
    // an obvious, aim-able target separated by the panel gap.
    lv_obj_set_style_bg_color(s_kbd, THEME_PANEL, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(s_kbd, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_kbd, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_kbd, THEME_TEXT, LV_PART_ITEMS);
    // Montserrat 24 (not the app's Styrene) because LVGL's Montserrat fonts
    // bundle the FontAwesome glyphs the keyboard needs for ⌫ (BACKSPACE) and
    // ✓ (OK); Styrene has no symbol range and would render them as tofu.
    lv_obj_set_style_text_font(s_kbd, &lv_font_montserrat_24, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_kbd, 8, LV_PART_ITEMS);

    // Pressed feedback: terra-cotta fill on any key (built-in state styling).
    lv_obj_set_style_bg_color(s_kbd, THEME_ACCENT, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_kbd, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_PRESSED);

    // OK is the only CHECKED key → it alone gets the steady terra-cotta fill.
    lv_obj_set_style_bg_color(s_kbd, THEME_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(s_kbd, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);

    // Give the textarea focus so the cursor appears.
    lv_group_t* indev_group = lv_group_get_default();
    if (indev_group) {
        lv_group_add_obj(indev_group, s_ta);
        lv_group_focus_obj(s_ta);
    }

    Serial.println("kbd_overlay: shown");
}

bool kbd_overlay_is_shown(void) {
    return s_overlay != nullptr;
}

void kbd_overlay_hide(void) {
    if (!s_overlay) return;
    lv_obj_delete(s_overlay);
    s_overlay  = nullptr;
    s_ta       = nullptr;
    s_kbd      = nullptr;
    s_cb       = nullptr;
    s_userdata = nullptr;
    Serial.println("kbd_overlay: hidden");
}
