#pragma once

// Radio coordinator. Enforces the invariant that AT MOST ONE of WiFi / BLE is
// enabled at a time — the ESP32-S3 shares a single 2.4 GHz radio between them
// and running both concurrently destabilizes connections.
//
// Every radio state change routes through here: the two settings-menu toggles
// and the single boot reconciliation. This is the only module that knows the
// cross-radio rules; settings.cpp stays a dumb NVS store.
namespace radio {
    // Enable WiFi (forces BLE off) or disable WiFi only (leaves BLE as-is —
    // both-off radio silence is allowed). Flips both the persisted NVS flag
    // and the runtime gate.
    void set_wifi(bool on);

    // Mirror image of set_wifi for the BLE radio.
    void set_ble(bool on);

    // Apply the persisted radio state at boot, enforcing the invariant.
    // Call AFTER both wifi_client::begin() and ble_init() have run.
    //   prov_mode == true  → SoftAP owns the radio during setup; force BLE off
    //                        and leave persisted toggles untouched (they apply
    //                        after the post-provisioning reboot).
    //   prov_mode == false → if both flags are on (default / pre-upgrade),
    //                        resolve via "WiFi if configured, else BLE" and
    //                        write the corrected loser back to NVS so the
    //                        device self-heals. Then apply both runtime gates.
    void reconcile_at_boot(bool prov_mode);
}
