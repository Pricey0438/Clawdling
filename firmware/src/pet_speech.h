#pragma once
#include <stdint.h>
#include "pet.h"

// Speech categories. SPEECH_NONE means "stay quiet this tick."
typedef enum {
    SPEECH_NONE = 0,
    SPEECH_GREETING,        // first display after a long idle / on screen-open
    SPEECH_TIME_NIGHT,      // local hour >= 23 OR < 5  (NOTE: UTC until tz is wired)
    SPEECH_TIME_MORNING,    // 5..11
    SPEECH_TIME_AFTERNOON,  // 12..17
    SPEECH_TIME_EVENING,    // 18..22
    SPEECH_HIGH_XP_DAY,     // earned XP recently (last_event_ts within last 30 min)
    SPEECH_LOW_STAT,        // any care stat below SAD_STAT_THRESHOLD (20)
    SPEECH_HUNGRY,          // satiety < 20 specifically
    SPEECH_HAPPY,           // all stats >= 70
    SPEECH_NEGLECTED,       // came back after long absence (stats > 50 but passive ts stale)
    SPEECH_VACATION,        // when vacation mode is active
    SPEECH_IDLE,            // catch-all when nothing else matches
    SPEECH__COUNT
} speech_category_t;

// Returns a line for the given category. Selection is deterministic:
// the same (category, salt) returns the same line. Use salt = pet_now()/60
// so lines vary minute-to-minute but stay stable during a brief window.
const char* pet_speech_pick(speech_category_t cat, uint32_t salt);

// Returns the category that best matches the current pet/world state.
// May return SPEECH_NONE if nothing notable is going on.
speech_category_t pet_speech_current_category(void);

// Number of lines available in a category (>= 1 for valid categories).
uint8_t pet_speech_count(speech_category_t cat);
