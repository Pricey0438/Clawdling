#include "settings.h"
#include <Preferences.h>
#include <Arduino.h>

#define NS        "settings"
#define K_BRIGHT  "bright"
#define K_PIN     "pin"
#define K_SPEECH  "speech"
#define K_RWIFI   "rwifi"
#define K_RBLE    "rble"

static uint8_t g_brightness         = SETTINGS_BRIGHTNESS_DEFAULT;
static int8_t  g_splash_pin         = SETTINGS_SPLASH_PIN_AUTO;
static bool    g_speech_enabled     = true;   // default on
static bool    g_radio_wifi_enabled = true;   // default on
static bool    g_radio_ble_enabled  = true;   // default on

namespace settings {

void init(void) {
    Preferences p;
    if (!p.begin(NS, /*readOnly=*/true)) {
        Serial.println("settings: no NVS namespace yet, using defaults");
        return;
    }
    g_brightness         = p.getUChar(K_BRIGHT, SETTINGS_BRIGHTNESS_DEFAULT);
    g_splash_pin         = (int8_t)p.getChar(K_PIN, SETTINGS_SPLASH_PIN_AUTO);
    g_speech_enabled     = p.getBool(K_SPEECH, true);
    g_radio_wifi_enabled = p.getBool(K_RWIFI,  true);
    g_radio_ble_enabled  = p.getBool(K_RBLE,   true);
    p.end();
    Serial.printf("settings: brightness=%u splash_pin=%d speech=%d wifi=%d ble=%d\n",
                  (unsigned)g_brightness, (int)g_splash_pin, (int)g_speech_enabled,
                  (int)g_radio_wifi_enabled, (int)g_radio_ble_enabled);
}

uint8_t brightness(void) { return g_brightness; }

void set_brightness(uint8_t v) {
    if (v == g_brightness) return;
    g_brightness = v;
    Preferences p;
    if (!p.begin(NS, /*readOnly=*/false)) return;
    p.putUChar(K_BRIGHT, v);
    p.end();
}

int8_t splash_pin(void) { return g_splash_pin; }

void set_splash_pin(int8_t v) {
    if (v == g_splash_pin) return;
    g_splash_pin = v;
    Preferences p;
    if (!p.begin(NS, /*readOnly=*/false)) return;
    p.putChar(K_PIN, v);
    p.end();
}

bool speech_enabled(void) { return g_speech_enabled; }

void set_speech_enabled(bool v) {
    if (v == g_speech_enabled) return;
    g_speech_enabled = v;
    Preferences p;
    if (!p.begin(NS, /*readOnly=*/false)) return;
    p.putBool(K_SPEECH, v);
    p.end();
}

bool radio_wifi_enabled(void) { return g_radio_wifi_enabled; }

void set_radio_wifi_enabled(bool v) {
    if (v == g_radio_wifi_enabled) return;
    g_radio_wifi_enabled = v;
    Preferences p;
    if (!p.begin(NS, /*readOnly=*/false)) return;
    p.putBool(K_RWIFI, v);
    p.end();
}

bool radio_ble_enabled(void) { return g_radio_ble_enabled; }

void set_radio_ble_enabled(bool v) {
    if (v == g_radio_ble_enabled) return;
    g_radio_ble_enabled = v;
    Preferences p;
    if (!p.begin(NS, /*readOnly=*/false)) return;
    p.putBool(K_RBLE, v);
    p.end();
}

void wipe(void) {
    Preferences p;
    if (!p.begin(NS, /*readOnly=*/false)) return;
    p.clear();
    p.end();
    g_brightness         = SETTINGS_BRIGHTNESS_DEFAULT;
    g_splash_pin         = SETTINGS_SPLASH_PIN_AUTO;
    g_speech_enabled     = true;
    g_radio_wifi_enabled = true;
    g_radio_ble_enabled  = true;
}

}  // namespace settings
