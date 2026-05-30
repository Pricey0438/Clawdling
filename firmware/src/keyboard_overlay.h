#pragma once
#include <stdint.h>

// Callback fired when the user confirms a name. `text` is null-terminated,
// non-null (but may be empty if the user cleared the field). `user_data` is
// whatever the caller passed to kbd_overlay_show().
typedef void (*kbd_done_cb_t)(const char* text, void* user_data);

// Show a full-screen modal keyboard overlay.
//   title        — shown at the top (e.g. "Name your pet")
//   initial_text — pre-fills the textarea (may be "" but not nullptr)
//   max_len      — maximum characters accepted (excluding null terminator)
//   on_done      — called with the final text when the user confirms
//   user_data    — passed through to on_done
//
// The overlay is modal: it blocks touch events on whatever is beneath it.
// Calling kbd_overlay_show() while one is already visible replaces it.
void kbd_overlay_show(const char* title,
                      const char* initial_text,
                      uint8_t     max_len,
                      kbd_done_cb_t on_done,
                      void*       user_data);

// Destroy the overlay without invoking on_done. Safe to call if none is shown.
void kbd_overlay_hide(void);

// True while the overlay is up. Used by the input dispatcher to gate the
// MID-cycle fallback so a button press during the hatch naming flow doesn't
// cycle the underlying screen out from under the keyboard.
bool kbd_overlay_is_shown(void);
