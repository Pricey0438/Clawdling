#pragma once
#include <stdint.h>
#include <lvgl.h>

// Mirror of the generated count in splash_animations.h. Kept here so other
// modules can static_assert against it without pulling in the full 180KB
// animation table. If splash_animations.h's #define changes, this guard
// lets the generated header re-define it without redefinition errors.
#ifndef SPLASH_ANIM_COUNT
#define SPLASH_ANIM_COUNT 13
#endif

// Initialize splash module. Creates the canvas widget inside `parent` and
// allocates the 480x480 pixel buffer (PSRAM).
void splash_init(lv_obj_t *parent);

// Advance animation frame if hold time elapsed. Call from main loop.
void splash_tick(void);

// Cycle to the next animation in the catalog.
void splash_next(void);

// Show/hide the splash container.
void splash_show(void);
void splash_hide(void);

// Pick the next animation matching the current usage-rate group.
// Called automatically by splash_show(); also exposed so other modules can
// trigger a re-pick when the rate group changes mid-display.
void splash_pick_for_current_rate(void);

// True when splash is currently rendering (used to gate re-picks).
bool splash_is_active(void);

// Root container (so ui.cpp can attach a click event).
lv_obj_t* splash_get_root(void);

// Render one animation frame into a caller-owned RGB565 buffer.
// dest:        pointer to dest_w * (GRID * cell_size) * 2 bytes of RGB565
// dest_w:      pixel width of dest buffer (>= GRID * cell_size)
// cell_size:   pixel size of each grid cell (>= 1)
// anim_idx:    index into splash_anims[] (0..SPLASH_ANIM_COUNT-1)
// frame_idx:   frame within the chosen animation (modulo'd internally)
// shiny:       when true, XOR each palette entry with 0x8410 (flips top bit
//              of R, G and B channels) for an alt-colour "shiny" look.
//              Defaults to false so existing callers are unaffected.
// dim:         when true, halve the R, G and B components of each palette
//              entry before rendering (sad/neglected appearance). Applied
//              after the shiny XOR so both transforms can compose.
//              Defaults to false so existing callers are unaffected.
// Returns true on success, false if inputs are invalid.
bool splash_render_to_buf(uint16_t* dest, int dest_w,
                          int cell_size,
                          uint16_t anim_idx, uint16_t frame_idx,
                          bool shiny = false, bool dim = false);

// Pick an animation index for the given "mood" group:
//   group 0 = idle/sleepy, 3 = heavy. Returns -1 if no animation matches.
int splash_pick_in_group(uint8_t group, uint8_t rotation_offset);
