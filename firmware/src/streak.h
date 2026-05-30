#pragma once
#include <stdint.h>

void     streak_init(void);              // load from NVS
void     streak_tick(void);              // detect day rollover; reset broken streaks

// Notifications — call from the corresponding action paths.
void     streak_note_care_action(void);     // called on any feed/play/pet success
void     streak_note_claude_session(void);  // called on first session-list with count>0 each tick

// Query
uint16_t streak_care_days(void);            // current care streak in days
uint16_t streak_usage_days(void);           // current usage streak in days
uint16_t streak_care_best(void);            // all-time best
uint16_t streak_usage_best(void);

// Test helpers — exposed permanently for serial commands.
void     streak_test_advance_days(uint16_t n);  // subtract n from last_day_idx, then tick
void     streak_test_set_days(uint16_t n);      // force g_care_days = g_usage_days = n; persist
