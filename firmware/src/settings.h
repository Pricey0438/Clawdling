#pragma once
#include <stdint.h>

#define SETTINGS_BRIGHTNESS_LOW     64
#define SETTINGS_BRIGHTNESS_MED     160
#define SETTINGS_BRIGHTNESS_HIGH    255
#define SETTINGS_BRIGHTNESS_DEFAULT SETTINGS_BRIGHTNESS_MED

#define SETTINGS_SPLASH_PIN_AUTO    (-1)

namespace settings {
    void    init(void);
    uint8_t brightness(void);
    void    set_brightness(uint8_t v);
    int8_t  splash_pin(void);
    void    set_splash_pin(int8_t v);
    // Speech bubbles on/off (P1-11). Default true. Gate for the pet-screen
    // greeting + the periodic 5-15 min auto-bubble.
    bool    speech_enabled(void);
    void    set_speech_enabled(bool v);
    // Radio toggles. Default both true. Persisted across reboots so the
    // user can leave their device WiFi/BLE-silent indefinitely without
    // re-toggling every boot. WiFi off = STA disconnect + WIFI_OFF mode;
    // BLE off = stop advertising (existing connections drain naturally).
    bool    radio_wifi_enabled(void);
    void    set_radio_wifi_enabled(bool v);
    bool    radio_ble_enabled(void);
    void    set_radio_ble_enabled(bool v);
    void    wipe(void);   // factory-reset path
}
