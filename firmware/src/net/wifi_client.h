#pragma once
#include <Arduino.h>

namespace wifi_client {
    void begin(const String& ssid, const String& pass);
    bool connected(void);
    int  rssi(void);                              // dBm; 0 if not connected
    void tick(void);                              // call from main loop
    // Radio gate. set_enabled(false) drops the STA association and puts
    // the WiFi peripheral into WIFI_OFF (cuts transmit and receive).
    // set_enabled(true) re-arms STA mode and reconnects with the SSID/pass
    // most recently passed to begin(). Persistence is the caller's job —
    // we only flip the runtime state. Safe to call repeatedly.
    void set_enabled(bool v);
    bool is_enabled(void);
}
