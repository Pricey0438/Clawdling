#pragma once
#include <stdint.h>
#include <stddef.h>

// Maximum BLE RX payload size. Exposed so callers of ble_snapshot_data()
// can size their local buffer to match.
#define BLE_BUF_SIZE 512

enum ble_state_t {
    BLE_STATE_INIT,
    BLE_STATE_ADVERTISING,
    BLE_STATE_CONNECTED,
    BLE_STATE_DISCONNECTED,
};

void ble_init(void);
void ble_tick(void);
ble_state_t ble_get_state(void);
const char* ble_get_device_name(void);
const char* ble_get_mac_address(void);
bool ble_has_received_data(void);
void ble_clear_bonds(void);
// Atomically copy the latest pending RX payload into the caller's buffer
// and clear the data-ready flag. Returns true if a payload was copied,
// false if nothing is pending. Replaces the older ble_has_data() +
// ble_get_data() pair, which exposed the raw rx_buf pointer and raced
// with NimBLE's onWrite callback (the host task can overwrite mid-parse).
bool ble_snapshot_data(char* dest, size_t dest_size);
void ble_send_ack(void);
void ble_send_nack(void);
void ble_request_refresh(void);
// Radio gate. ble_set_enabled(false) stops advertising so the device is
// invisible to new scanners (the user's "always broadcasting" complaint);
// existing connections persist until the peer drops them. ble_set_enabled
// (true) re-starts advertising. Safe to call repeatedly. Persistence is
// the caller's job — settings::set_radio_ble_enabled writes NVS.
void ble_set_enabled(bool v);
bool ble_is_enabled(void);
