#pragma once
#include <Arduino.h>

namespace net_daemon {
    // Reads config, then spawns a Core-0 FreeRTOS task that owns the
    // HTTPS poll. The loop task never blocks on TLS. Idempotent.
    void init(void);

    // Last successful poll within 30s? Cheap, lock-free (single 32-bit
    // read is atomic on ESP32-S3). Safe to call every loop iteration.
    bool state_fresh(void);

    // Newest JSON snapshot from the poll task, or "" if none yet.
    // Returns a pointer to a loop-task-private buffer that's safe to
    // hold until the next call to latest(). Internally takes the
    // shared-state lock to copy the producer buffer into the snapshot.
    const char* latest(void);

    // Queue a POST /refresh for the poll task to send on its next tick.
    void request_refresh(void);

    // millis() timestamp of the most recent successful poll. 0 if never.
    // Used by WiFi info drill to display "Last poll Xs ago".
    uint32_t last_apply_ts(void);
}
