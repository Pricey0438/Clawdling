#include "radio.h"
#include "settings.h"
#include "ble.h"
#include "net/wifi_client.h"
#include "net/config_store.h"
#include <Arduino.h>

namespace radio {

void set_wifi(bool on) {
    if (on) {
        settings::set_radio_wifi_enabled(true);
        settings::set_radio_ble_enabled(false);
        wifi_client::set_enabled(true);
        ble_set_enabled(false);
    } else {
        settings::set_radio_wifi_enabled(false);
        wifi_client::set_enabled(false);
    }
}

void set_ble(bool on) {
    if (on) {
        settings::set_radio_ble_enabled(true);
        settings::set_radio_wifi_enabled(false);
        ble_set_enabled(true);
        wifi_client::set_enabled(false);
    } else {
        settings::set_radio_ble_enabled(false);
        ble_set_enabled(false);
    }
}

void reconcile_at_boot(bool prov_mode) {
    if (prov_mode) {
        // SoftAP owns the radio during setup — keep BLE dark to avoid the
        // WiFi-AP + BLE coexistence. Persisted toggles take effect after the
        // post-provisioning reboot, when creds exist.
        ble_set_enabled(false);
        // Note: wifi_client::set_enabled is intentionally NOT called here —
        // the SoftAP is managed by provisioning::, not wifi_client, so it
        // stays up on its own. We only need to ensure BLE is dark.
        Serial.println("radio: provisioning — BLE forced off");
        return;
    }

    bool w = settings::radio_wifi_enabled();
    bool b = settings::radio_ble_enabled();

    if (w && b) {
        // Illegal both-on (fresh-device default, or any device upgrading from
        // before the invariant). Resolve via tiebreak and persist the loser so
        // the device self-heals permanently on this first boot.
        if (cfg::has_wifi()) {
            settings::set_radio_ble_enabled(false);
            b = false;
            Serial.println("radio: reconcile both-on -> wifi (configured)");
        } else {
            settings::set_radio_wifi_enabled(false);
            w = false;
            Serial.println("radio: reconcile both-on -> ble (no creds)");
        }
    }

    wifi_client::set_enabled(w);
    ble_set_enabled(b);
    Serial.printf("radio: active wifi=%d ble=%d\n", (int)w, (int)b);
}

}  // namespace radio
