#include "ble.h"
#include "version.h"
#include <Arduino.h>
#include <string.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

#define DEVICE_NAME "Claude Controller"

// Custom GATT UUIDs for data channel
#define SERVICE_UUID        "4c41555a-4465-7669-6365-000000000001"
#define RX_CHAR_UUID        "4c41555a-4465-7669-6365-000000000002"  // host writes here
#define TX_CHAR_UUID        "4c41555a-4465-7669-6365-000000000003"  // device ack/nack notifies
#define REQ_CHAR_UUID       "4c41555a-4465-7669-6365-000000000004"  // device-initiated refresh request
#define VERSION_CHAR_UUID   "4c41555a-4465-7669-6365-000000000005"  // device version metadata (read-only)

// BLE_BUF_SIZE is defined in ble.h so callers of ble_snapshot_data() can
// size their buffer to match.

static NimBLEServer* server = nullptr;
static NimBLECharacteristic* tx_char = nullptr;
static NimBLECharacteristic* rx_char = nullptr;
static NimBLECharacteristic* req_char = nullptr;

// Set from onConnect / onDisconnect (NimBLE host task) and from ble_clear_bonds;
// read from ble_tick (Arduino loop task) and ble_get_state (main loop).
// Single-byte writes are atomic on ESP32-S3, but `volatile` is needed to stop
// the compiler caching the read in a register across the loop iteration.
static volatile ble_state_t state = BLE_STATE_INIT;
static volatile bool need_advertise = false;
static char rx_buf[BLE_BUF_SIZE];
static volatile bool data_ready = false;
static volatile bool has_received_data = false;
static char mac_str[18];

// User-controlled radio gate. When false, advertising stays stopped even
// if NimBLE's onDisconnect handler would otherwise re-arm need_advertise
// (which it does on every disconnect). Defaults true; radio::reconcile_at_boot()
// (run just after ble_init() returns) calls ble_set_enabled(false) if the
// persisted setting says BLE is off — so ble_init() always advertises briefly
// before the gate is applied, which is harmless.
static bool s_radio_enabled = true;
// True once ble_init() has brought up the NimBLE stack. The radio coordinator
// may gate BLE (ble_set_enabled(false)) BEFORE ble_init() runs — in the
// single-radio boot path BLE is never initialised when WiFi wins. Touching
// NimBLE (getAdvertising/stop) before init is unsafe, so guard on this.
static bool s_ble_initialized = false;

// Guards rx_buf and the data_ready flag against the NimBLE host task
// racing the main loop. portMUX is a cross-core spinlock (the BLE callback
// can run on either core); held only long enough to memcpy <=512 bytes so
// it's cheap.
static portMUX_TYPE rx_mux = portMUX_INITIALIZER_UNLOCKED;

// Deferred-BLE-write flags. Both BLE characteristic callbacks (onWrite,
// onSubscribe) run on the NimBLE host task with internal locks held; firing
// a notify() inline can deadlock under some NimBLE versions. These flags
// capture the intent; ble_tick() drains them from the main-loop task where
// notifies are safe. Two separate flags rather than a single slot so a
// refresh-then-overflow sequence between ticks can't lose one event.
static volatile bool pending_refresh = false;          // onSubscribe → ble_request_refresh()
static volatile bool pending_nack_truncated = false;   // onWrite saw an oversized payload

// P2-8: deferred diagnostic Serial.printf out of NimBLE callbacks.
// Serial writes can block briefly when the UART buffer is full; from a
// NimBLE host-task callback that stalls the BLE stack. Callbacks now
// snprintf into these small buffers under log_mux; ble_tick() emits and
// clears them from the main-loop task. Only the latest event of each kind
// is retained — back-to-back connects coalesce, which is fine for
// diagnostics.
static portMUX_TYPE log_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool log_connect_pending    = false;
static volatile bool log_disconnect_pending = false;
static volatile bool log_subscribe_pending  = false;
static char log_connect_buf[96];
static char log_disconnect_buf[64];
static char log_subscribe_buf[64];

static void start_advertising() {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->reset();
    // Primary advertising packet (≤31 bytes): flags (3) + name "Claude
    // Controller" (19) = 22 bytes. The custom 128-bit data-service UUID
    // goes in the scan response (active scanners only) to keep the primary
    // packet small and increase discovery range.
    adv->setName(DEVICE_NAME);
    // Scan response carries the 128-bit custom data-service UUID for active
    // scanners (the host daemon scans actively).
    NimBLEAdvertisementData scanResp;
    scanResp.setCompleteServices(NimBLEUUID(SERVICE_UUID));
    adv->setScanResponseData(scanResp);
    adv->enableScanResponse(true);
    bool ok = adv->start();
    // Only reflect ADVERTISING in the UI state when no client is connected.
    // With MAX_CONNECTIONS=2, onConnect re-advertises to fill the second slot;
    // without this guard the UI would flip CONNECTED → ADVERTISING on every
    // first connect and never come back until a second client arrived.
    if (!server || server->getConnectedCount() == 0) {
        state = BLE_STATE_ADVERTISING;
    }
    Serial.printf("BLE: advertising start=%s (connected=%u)\n",
        ok ? "OK" : "FAILED",
        server ? (unsigned)server->getConnectedCount() : 0);
}

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
        state = BLE_STATE_CONNECTED;
        // P2-8: defer the Serial.printf — see log_mux comment above.
        portENTER_CRITICAL(&log_mux);
        snprintf(log_connect_buf, sizeof(log_connect_buf),
            "BLE: connected from %s (active=%u)",
            info.getAddress().toString().c_str(),
            (unsigned)s->getConnectedCount());
        log_connect_pending = true;
        portEXIT_CRITICAL(&log_mux);
        // Keep advertising while a connection slot is still free so a second
        // central (e.g. the host daemon alongside an OS-held HID link) can
        // discover and connect. NimBLE auto-stops advertising on each accept.
        if (s->getConnectedCount() < CONFIG_BT_NIMBLE_MAX_CONNECTIONS) {
            need_advertise = true;
        }
    }

    void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
        // Only flip the UI state to DISCONNECTED when the last client leaves.
        if (s->getConnectedCount() == 0) state = BLE_STATE_DISCONNECTED;
        need_advertise = true;
        // P2-8: defer the Serial.printf — see log_mux comment above.
        portENTER_CRITICAL(&log_mux);
        snprintf(log_disconnect_buf, sizeof(log_disconnect_buf),
            "BLE: disconnected (reason=%d, remaining=%u)",
            reason, (unsigned)s->getConnectedCount());
        log_disconnect_pending = true;
        portEXIT_CRITICAL(&log_mux);
    }

};

class RxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& info) override {
        std::string val = chr->getValue();
        size_t len = val.length();
        if (len >= BLE_BUF_SIZE) {
            // Almost certainly truncated by NimBLE's reassembly or the peer's
            // chunking. Don't try to parse — set the deferred-NACK flag so the
            // next ble_tick fires a notify on tx_char from a safe context.
            pending_nack_truncated = true;
            return;
        }
        portENTER_CRITICAL(&rx_mux);
        memcpy(rx_buf, val.c_str(), len);
        rx_buf[len] = '\0';
        data_ready = true;
        has_received_data = true;
        portEXIT_CRITICAL(&rx_mux);
    }
};

// When the daemon enables notifications on the refresh char, ask for data
// if we have none yet. Firing on subscribe (not on connect) ensures the
// notification isn't dropped before the daemon's CCCD write completes.
class ReqCallbacks : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic* chr, NimBLEConnInfo& info, uint16_t subValue) override {
        // P2-8: defer the Serial.printf — see log_mux comment above.
        portENTER_CRITICAL(&log_mux);
        snprintf(log_subscribe_buf, sizeof(log_subscribe_buf),
            "BLE: req_char onSubscribe subValue=%u has_data=%d",
            (unsigned)subValue, has_received_data ? 1 : 0);
        log_subscribe_pending = true;
        portEXIT_CRITICAL(&log_mux);
        if (subValue != 0 && !has_received_data) {
            // Defer the notify — calling ble_request_refresh() (which calls
            // req_char->notify()) from inside a NimBLE callback can deadlock
            // under some NimBLE versions. ble_tick drains this flag from the
            // main-loop task where notifies are safe.
            pending_refresh = true;
        }
    }
};

void ble_init(void) {
    NimBLEDevice::init(DEVICE_NAME);
    NimBLEDevice::setSecurityAuth(true, false, true);  // bonding, no MITM, SC

    // Format MAC address
    NimBLEAddress addr = NimBLEDevice::getAddress();
    snprintf(mac_str, sizeof(mac_str), "%s", addr.toString().c_str());
    for (int i = 0; mac_str[i]; i++) {
        if (mac_str[i] >= 'a' && mac_str[i] <= 'f') mac_str[i] -= 32;
    }

    server = NimBLEDevice::createServer();
    static ServerCallbacks serverCb;
    server->setCallbacks(&serverCb);

    // --- Custom data service ---
    NimBLEService* svc = server->createService(SERVICE_UUID);

    rx_char = svc->createCharacteristic(
        RX_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    static RxCallbacks rxCb;
    rx_char->setCallbacks(&rxCb);

    tx_char = svc->createCharacteristic(
        TX_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    req_char = svc->createCharacteristic(
        REQ_CHAR_UUID,
        NIMBLE_PROPERTY::NOTIFY
    );
    static ReqCallbacks reqCb;
    req_char->setCallbacks(&reqCb);

    NimBLECharacteristic* version_char = svc->createCharacteristic(
        VERSION_CHAR_UUID,
        NIMBLE_PROPERTY::READ
    );
    {
        char ver_buf[64];
        snprintf(ver_buf, sizeof(ver_buf), "%s %s %s",
                 FW_VERSION_STR, FW_VERSION, FW_BOARD_ENV);
        version_char->setValue((uint8_t*)ver_buf, strlen(ver_buf));
    }

    svc->start();
    server->start();
    s_ble_initialized = true;
    start_advertising();

    Serial.printf("BLE: init complete, MAC=%s\n", mac_str);
}

void ble_tick(void) {
    if (need_advertise) {
        need_advertise = false;
        if (s_radio_enabled) start_advertising();
    }
    // Drain deferred BLE writes set by NimBLE callback context. Each flag is
    // coalesce-safe within its kind: repeated refreshes collapse to one
    // (has_received_data guards re-arming); repeated truncation NACKs collapse
    // to one (the daemon retries on its next push).
    if (pending_refresh) {
        pending_refresh = false;
        ble_request_refresh();
    }
    if (pending_nack_truncated) {
        pending_nack_truncated = false;
        ble_send_nack();
    }

    // P2-8: drain deferred diagnostic Serial.printf payloads captured in
    // NimBLE callbacks. Snapshot under log_mux into a stack buffer, then
    // print outside the critical section (Serial.println itself can block).
    if (log_connect_pending) {
        char line[sizeof(log_connect_buf)];
        portENTER_CRITICAL(&log_mux);
        memcpy(line, log_connect_buf, sizeof(line));
        log_connect_pending = false;
        portEXIT_CRITICAL(&log_mux);
        Serial.println(line);
    }
    if (log_disconnect_pending) {
        char line[sizeof(log_disconnect_buf)];
        portENTER_CRITICAL(&log_mux);
        memcpy(line, log_disconnect_buf, sizeof(line));
        log_disconnect_pending = false;
        portEXIT_CRITICAL(&log_mux);
        Serial.println(line);
    }
    if (log_subscribe_pending) {
        char line[sizeof(log_subscribe_buf)];
        portENTER_CRITICAL(&log_mux);
        memcpy(line, log_subscribe_buf, sizeof(line));
        log_subscribe_pending = false;
        portEXIT_CRITICAL(&log_mux);
        Serial.println(line);
    }
}

ble_state_t ble_get_state(void) {
    return state;
}

bool ble_has_received_data(void) {
    return has_received_data;
}

const char* ble_get_device_name(void) {
    return DEVICE_NAME;
}

const char* ble_get_mac_address(void) {
    return mac_str;
}

void ble_clear_bonds(void) {
    NimBLEDevice::deleteAllBonds();
    Serial.println("BLE: bonds cleared");
    // Read the live connection count from NimBLE rather than our cached
    // `state` flag — the disconnect callback can flip `state` between any
    // two reads. getConnectedCount + getPeerInfo are consistent within one
    // call into NimBLE's host stack, so the index-0 entry is valid as
    // long as the count is > 0.
    if (server && server->getConnectedCount() > 0) {
        NimBLEConnInfo info = server->getPeerInfo(0);
        server->disconnect(info.getConnHandle());
    }
    need_advertise = true;
}

bool ble_snapshot_data(char* dest, size_t dest_size) {
    if (!dest || dest_size == 0) return false;
    bool ok = false;
    portENTER_CRITICAL(&rx_mux);
    if (data_ready) {
        size_t n = strnlen(rx_buf, BLE_BUF_SIZE - 1);
        if (n >= dest_size) n = dest_size - 1;
        memcpy(dest, rx_buf, n);
        dest[n] = '\0';
        data_ready = false;
        ok = true;
    }
    portEXIT_CRITICAL(&rx_mux);
    return ok;
}

void ble_send_ack(void) {
    if (state == BLE_STATE_CONNECTED && tx_char) {
        tx_char->setValue("{\"ack\":true}");
        tx_char->notify();
    }
}

void ble_send_nack(void) {
    if (state == BLE_STATE_CONNECTED && tx_char) {
        tx_char->setValue("{\"err\":true}");
        tx_char->notify();
    }
}

void ble_request_refresh(void) {
    if (state == BLE_STATE_CONNECTED && req_char) {
        uint8_t v = 0x01;
        req_char->setValue(&v, 1);
        req_char->notify();
        Serial.println("BLE: refresh requested");
    }
}

void ble_set_enabled(bool v) {
    if (v == s_radio_enabled) return;
    s_radio_enabled = v;
    if (!v) {
        Serial.println("BLE: radio OFF (user toggle)");
        if (s_ble_initialized) {
            NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
            if (adv) adv->stop();
        }
        need_advertise = false;
        if (state != BLE_STATE_CONNECTED) state = BLE_STATE_DISCONNECTED;
        return;
    }
    Serial.println("BLE: radio ON (user toggle)");
    if (s_ble_initialized) start_advertising();
}

bool ble_is_enabled(void) { return s_radio_enabled; }
