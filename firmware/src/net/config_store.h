#pragma once
#include <Arduino.h>

namespace cfg {
    // Max field lengths enforced by save_all() and callers. Preferences::putString
    // silently truncates on overflow → device-side write "succeeds" but stored
    // value is short, surfacing only as a later HTTPS 401. Reject overlong
    // input up-front instead.
    static constexpr size_t CFG_MAX_SSID  = 32;
    static constexpr size_t CFG_MAX_PASS  = 64;
    static constexpr size_t CFG_MAX_URL   = 256;
    static constexpr size_t CFG_MAX_TOKEN = 512;

    void init(void);                              // open NVS namespace "clawdmeter"
    bool has_wifi(void);                          // returns true if ssid+pass+url+token are all set
    bool load_wifi(String& ssid, String& pass);
    bool load_daemon(String& url, String& token);
    bool save_all(const String& ssid, const String& pass,
                  const String& url, const String& token);
    void wipe(void);                              // factory reset

    // Wipe ONLY WiFi + daemon keys (ssid, pass, url, token). Used by the
    // menu's "Switch network" action so the user can change networks
    // without losing pet state or other settings.
    void clear_wifi_only(void);
}
