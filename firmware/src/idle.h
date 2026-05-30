#pragma once
#include <stdbool.h>

void idle_init(void);
void idle_tick(void);
void idle_note_activity(void);

// Returns true if this press was consumed as a wake-up (caller MUST skip the
// button's normal action). Returns false when already awake — also notes the
// activity, so callers don't need a separate idle_note_activity() call.
bool idle_consume_wake_press(void);

// Touch should NOT count as activity (avoids accidental wakes from pets,
// sleeves, etc.). Callers use this to silently drop touch events while the
// panel is dark.
bool idle_is_asleep(void);

// Fast-sleep mode: shorten the idle timeout to 30s and ignore the USB
// keep-awake exemption. Used while the device is locked (see ui_set_locked)
// so a locked-and-shelved device powers down its panel quickly regardless
// of power source. Off restores the normal 30-minute timeout.
void idle_set_fast_sleep(bool enable);
