#pragma once
#include <Arduino.h>

namespace provisioning {
    void begin(void);          // start SoftAP + DNS + HTTP form
    bool active(void);         // true while in AP mode
    void tick(void);           // call from main loop
    const String& ap_ssid(void);  // SSID of the provisioning AP (e.g. "Clawdmeter-AB12")
}
