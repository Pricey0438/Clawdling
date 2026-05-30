#pragma once

// Verify that the running firmware matches the connected hardware.
// Probes I2C @ 0x20 (the XCA9554 IO expander present on AMOLED-1.8 only)
// and compares the result against the compile-time BOARD_HAS_IO_EXPANDER
// flag from board.h. On mismatch, logs FATAL to serial (display init has
// not run yet — serial is the only signal) and halts in a ticker loop.
//
// MUST be called from setup() BEFORE board_init(), so the I2C bus is in
// a clean pre-init state. Returns true on match; on mismatch, never
// returns (busy-loops to keep the fatal message visible to anyone with
// a USB cable attached).
//
// Does its own Wire.begin()/Wire.end() so board_init()'s own
// Wire.begin() remains the canonical one.
bool board_self_id_verify_or_halt(void);
