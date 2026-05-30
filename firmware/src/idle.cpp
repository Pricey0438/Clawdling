#include <Arduino.h>
#include "idle.h"
#include "idle_cfg.h"
#include "settings.h"
#include "hal/display_hal.h"
#include "hal/power_hal.h"

// "Awake" brightness target — pulled from user-tunable settings so the
// menu's Brightness drill can dim the panel without rebuilding idle's
// fade logic. Falls back to DISPLAY_DEFAULT_BRIGHTNESS if settings hasn't
// loaded yet (g_brightness defaults to SETTINGS_BRIGHTNESS_DEFAULT = 160,
// so the fallback is only hit if 0 was somehow persisted).
static inline uint8_t awake_brightness(void) {
    uint8_t b = settings::brightness();
    return b ? b : (uint8_t)DISPLAY_DEFAULT_BRIGHTNESS;
}

enum IdleState {
    STATE_AWAKE,
    STATE_FADING_OUT,
    STATE_ASLEEP,
    STATE_FADING_IN,
};

static IdleState state = STATE_AWAKE;
static uint32_t last_activity_ms = 0;
static uint32_t fade_started_ms  = 0;
static uint32_t fade_last_step_ms = 0;
static uint8_t  fade_from = DISPLAY_DEFAULT_BRIGHTNESS;   // overwritten by begin_fade() before first use
static uint8_t  fade_to   = 0;
static bool     s_fast_sleep = false;

// 30s timeout used by fast-sleep mode (lock screen). Hardcoded — user
// requested this in 2026-05-27 review.
static constexpr uint32_t IDLE_FAST_TIMEOUT_MS = 30UL * 1000UL;

static void apply_brightness(uint8_t b) {
    display_hal_set_brightness(b);
}

static void begin_fade(uint8_t to, uint32_t now) {
    fade_from = (to == 0) ? awake_brightness() : 0;
    fade_to   = to;
    fade_started_ms = now;
    fade_last_step_ms = now;
}

void idle_init(void) {
    state = STATE_AWAKE;
    last_activity_ms = millis();
    apply_brightness(awake_brightness());
}

void idle_set_fast_sleep(bool enable) {
    if (enable == s_fast_sleep) return;
    s_fast_sleep = enable;
    // Reset the inactivity timer on transition so the new timeout starts
    // counting from "now" rather than firing immediately if the device has
    // been idle.
    last_activity_ms = millis();
}

void idle_note_activity(void) {
    last_activity_ms = millis();
    if (state == STATE_FADING_IN) return;
    if (state == STATE_AWAKE) return;
    // Asleep/fading-out shouldn't reach here in normal flow (callers gate via
    // idle_consume_wake_press first), but if it does: trigger a wake.
    begin_fade(awake_brightness(), last_activity_ms);
    state = STATE_FADING_IN;
}

bool idle_consume_wake_press(void) {
    if (state == STATE_ASLEEP || state == STATE_FADING_OUT) {
        uint32_t now = millis();
        last_activity_ms = now;
        begin_fade(awake_brightness(), now);
        state = STATE_FADING_IN;
        return true;
    }
    if (state == STATE_FADING_IN) {
        // Mid-wake — still swallow this press; the user shouldn't get a
        // half-wake half-action surprise.
        last_activity_ms = millis();
        return true;
    }
    last_activity_ms = millis();
    return false;
}

bool idle_is_asleep(void) {
    return state == STATE_ASLEEP || state == STATE_FADING_OUT;
}

void idle_tick(void) {
    uint32_t now = millis();

    // While on USB power (if configured), don't sleep — and wake from sleep
    // when power comes back. Treats USB-in as continuous activity. Fast-sleep
    // mode (lock screen) skips this exemption so a locked device powers down
    // regardless of power source.
    if (!s_fast_sleep && !IDLE_SLEEP_WHEN_CHARGING && power_hal_is_vbus_in()) {
        last_activity_ms = now;
        if (state == STATE_ASLEEP || state == STATE_FADING_OUT) {
            begin_fade(awake_brightness(), now);
            state = STATE_FADING_IN;
        }
    }

    const uint32_t timeout = s_fast_sleep ? IDLE_FAST_TIMEOUT_MS : IDLE_TIMEOUT_MS;

    switch (state) {
    case STATE_AWAKE:
        if (now - last_activity_ms >= timeout) {
            Serial.printf("[idle] fade-out (timeout=%lums, fast=%d)\n",
                          (unsigned long)timeout, s_fast_sleep ? 1 : 0);
            begin_fade(0, now);
            state = STATE_FADING_OUT;
        }
        break;

    case STATE_FADING_OUT:
    case STATE_FADING_IN: {
        if (now - fade_last_step_ms < IDLE_FADE_STEP_MS) break;
        fade_last_step_ms = now;
        uint32_t dur = (state == STATE_FADING_OUT) ? IDLE_FADE_OUT_MS : IDLE_FADE_IN_MS;
        uint32_t elapsed = now - fade_started_ms;
        if (elapsed >= dur) {
            apply_brightness(fade_to);
            bool went_asleep = (state == STATE_FADING_OUT);
            state = went_asleep ? STATE_ASLEEP : STATE_AWAKE;
            Serial.println(went_asleep ? "[idle] asleep" : "[idle] awake");
        } else {
            // Linear interpolation fade_from -> fade_to over dur ms.
            int32_t span = (int32_t)fade_to - (int32_t)fade_from;
            int32_t b = (int32_t)fade_from + (span * (int32_t)elapsed) / (int32_t)dur;
            if (b < 0) b = 0;
            if (b > 255) b = 255;
            apply_brightness((uint8_t)b);
        }
        break;
    }

    case STATE_ASLEEP:
        break;
    }
}
