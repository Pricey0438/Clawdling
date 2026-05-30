#include "net_daemon.h"
#include "wifi_client.h"
#include "http_client.h"
#include "config_store.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/portmacro.h>

namespace net_daemon {

static const uint32_t POLL_INTERVAL_MS = 5000;
static const uint32_t FRESH_WINDOW_MS  = 30000;
static const size_t   STATE_BUF_SIZE   = 2048;

// Self-heal watchdog: if WiFi has been up for STUCK_TIMEOUT_MS and we
// still haven't had a successful poll in that window, reboot the device.
// Recovers from singleton-TLS-client wedges, stuck WiFi associations
// (link-up but no traffic), and any other state we can't see from here.
// 90 s is long enough that a transient daemon hiccup or roaming event
// won't trigger a restart, but short enough that a remote user power-
// cycling is not the only recovery path.
static const uint32_t STUCK_TIMEOUT_MS = 90000;

// Poll task lives on Core 0 (alongside NimBLE host) so the Arduino loop
// on Core 1 stays responsive. NimBLE's host task is event-driven and
// spends most of its time blocked; our poll task is the same shape —
// blocked on vTaskDelay between 1 s ticks, with the TLS handshake
// itself being the only CPU-bound burst. 16 KB stack covers the TLS
// handshake (~10 KB peak) + HTTP response copy + headroom. Priority 3
// keeps us below NimBLE host (~5) and above idle.
static const UBaseType_t POLL_TASK_PRIO   = 3;
static const uint32_t    POLL_TASK_STACK  = 16384;
static const BaseType_t  POLL_TASK_CORE   = 0;
static const TickType_t  POLL_TICK_PERIOD = pdMS_TO_TICKS(1000);

// Config snapshot: read once at init() then owned by the poll task. The
// loop task never writes these after start(), so no lock needed.
static String s_url;
static String s_token;
static bool   s_configured = false;

// Shared state written by the poll task, read by the loop task.
// Guarded by s_mux (cross-core spinlock — same pattern as ble.cpp's
// rx_mux: held only long enough to memcpy <=2 KB).
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char     s_latest[STATE_BUF_SIZE] = {0};
static uint32_t s_last_ok_ms = 0;

// Flags written by both tasks. uint32 reads/writes are atomic on the
// ESP32-S3 (single 32-bit aligned word, single bus cycle); volatile is
// enough to keep the compiler from caching them in registers.
static volatile bool s_pending_refresh = false;
static volatile bool s_was_connected_last_tick = false;

// Watchdog state: timestamp of when WiFi most recently transitioned to
// connected. Reset on every disconnect so the watchdog gives the new
// link a full STUCK_TIMEOUT_MS to produce a successful poll.
static uint32_t s_wifi_up_since_ms = 0;

// Loop-task-private snapshot buffer. latest() copies s_latest into here
// under the lock and returns a stable pointer. Only the loop task reads
// from it, so it doesn't need its own lock.
static char s_snapshot[STATE_BUF_SIZE] = {0};

static TaskHandle_t s_task = nullptr;

static void poll_task(void*) {
    for (;;) {
        vTaskDelay(POLL_TICK_PERIOD);

        if (!s_configured) continue;
        if (!wifi_client::connected()) {
            s_was_connected_last_tick = false;
            s_wifi_up_since_ms = 0;
            continue;
        }
        // WiFi-edge refresh: queue a /refresh on (re)connect, mirroring the
        // BLE onSubscribe behaviour.
        if (!s_was_connected_last_tick) {
            s_pending_refresh = true;
            s_was_connected_last_tick = true;
            s_wifi_up_since_ms = millis();
        }

        // Watchdog: recover a GENUINE wedge — we had at least one successful
        // poll and then went silent for the whole timeout (the singleton TLS
        // client can stick). Reboot to recover. Checked before the throttle so
        // it fires even on ticks we'd skip.
        //
        // CRITICAL: only reboot when s_last_ok_ms != 0 (we HAD a success). If
        // the daemon was never reachable, a reboot can't fix that — it just
        // reboot-loops every STUCK_TIMEOUT_MS, and repeated resets corrupt NVS
        // (lost WiFi creds → device hard-stuck on the provisioning QR). Being
        // offline / unable to reach the daemon must mean stale-then-empty data,
        // never a reboot loop.
        if (s_wifi_up_since_ms != 0 && s_last_ok_ms != 0) {
            uint32_t since_ok = millis() - s_last_ok_ms;
            if (since_ok >= STUCK_TIMEOUT_MS) {
                Serial.printf("net: watchdog reboot — %us since last successful poll (link wedged)\n",
                              since_ok / 1000);
                Serial.flush();
                ESP.restart();
            }
        }

        if (s_pending_refresh) {
            s_pending_refresh = false;
            http_client::post_refresh(s_url, s_token);
            // Fall through and poll for fresh state immediately.
        } else {
            // Throttle to POLL_INTERVAL_MS. Read s_last_ok_ms snapshot
            // outside the critical section — single uint32 read is atomic.
            uint32_t last = s_last_ok_ms;
            uint32_t now  = millis();
            if (last != 0 && (now - last) < POLL_INTERVAL_MS) continue;
        }

        // Stage the response into a task-local buffer first so the lock
        // window only covers the memcpy into the shared buffer.
        static char buf[STATE_BUF_SIZE];
        if (http_client::get_state(s_url, s_token, buf, sizeof(buf))) {
            size_t n = strnlen(buf, STATE_BUF_SIZE - 1);
            uint32_t now = millis();
            portENTER_CRITICAL(&s_mux);
            memcpy(s_latest, buf, n);
            s_latest[n] = '\0';
            s_last_ok_ms = now;
            portEXIT_CRITICAL(&s_mux);
        }
    }
}

void init(void) {
    s_configured = cfg::load_daemon(s_url, s_token);
    if (s_task == nullptr) {
        xTaskCreatePinnedToCore(
            poll_task,
            "net_poll",
            POLL_TASK_STACK,
            nullptr,
            POLL_TASK_PRIO,
            &s_task,
            POLL_TASK_CORE);
    }
}

bool state_fresh(void) {
    // Single 32-bit aligned read is atomic on ESP32-S3; no lock needed
    // for a freshness check that's called every loop iteration.
    uint32_t last = s_last_ok_ms;
    if (last == 0) return false;
    return (millis() - last) <= FRESH_WINDOW_MS;
}

uint32_t last_apply_ts(void) { return s_last_ok_ms; }

const char* latest(void) {
    // Snapshot under the lock so apply_payload sees a consistent
    // (non-torn) JSON string even if the poll task is mid-write.
    portENTER_CRITICAL(&s_mux);
    size_t n = strnlen(s_latest, STATE_BUF_SIZE - 1);
    memcpy(s_snapshot, s_latest, n);
    s_snapshot[n] = '\0';
    portEXIT_CRITICAL(&s_mux);
    return s_snapshot;
}

void request_refresh(void) {
    s_pending_refresh = true;
}

}  // namespace net_daemon
