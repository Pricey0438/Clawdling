#include "board_self_id.h"
#include "board.h"          // pulls in this board's BOARD_HAS_IO_EXPANDER value
#include "version.h"
#include <Arduino.h>
#include <Wire.h>

// Both supported boards put I2C on SDA=15 / SCL=14. We could read these
// from board.h (already included for BOARD_HAS_IO_EXPANDER below) but
// keep them inline so a future third board with different pins forces
// an explicit decision here rather than silently picking one board's
// pins via #include order.
static constexpr int PROBE_SDA = 15;
static constexpr int PROBE_SCL = 14;
static constexpr uint8_t IO_EXPANDER_ADDR = 0x20;

static bool probe_io_expander(void) {
    Wire.begin(PROBE_SDA, PROBE_SCL);
    Wire.beginTransmission(IO_EXPANDER_ADDR);
    uint8_t err = Wire.endTransmission();
    Wire.end();
    // 0 = ACK received (device present). Anything else = no ACK / bus error.
    return err == 0;
}

bool board_self_id_verify_or_halt(void) {
    bool present = probe_io_expander();
    bool expected = (bool)BOARD_HAS_IO_EXPANDER;

    if (present == expected) {
        Serial.printf("[board] expected=%s io_expander=%s OK\n",
                      FW_BOARD_ENV, present ? "present" : "absent");
        return true;
    }

    // Mismatch — fatal.
    Serial.printf("[board] expected=%s io_expander=%s MISMATCH FATAL\n",
                  FW_BOARD_ENV, present ? "present" : "absent");
    Serial.printf("[board] this firmware (%s) is for the %s board\n",
                  FW_BOARD_ENV,
                  expected ? "AMOLED-1.8 (XCA9554 expander)" : "AMOLED-2.16 (no expander)");
    Serial.printf("[board] detected hardware looks like %s\n",
                  present ? "AMOLED-1.8 (expander present)" : "AMOLED-2.16 (no expander)");
    // Pick the env the user should flash to fix this — the OPPOSITE of
    // what's currently running. If a future third board lands, this lookup
    // needs to grow.
    const char* correct_env = expected
        ? "waveshare_amoled_216"   // running 1.8 firmware, hw has no expander → flash 2.16
        : "waveshare_amoled_18";   // running 2.16 firmware, hw has expander  → flash 1.8

    Serial.printf("[board] flash the correct env: %s\n", correct_env);

    while (1) {
        Serial.printf("[board] WRONG FIRMWARE — flash %s\n", correct_env);
        delay(2000);
    }
    return false;  // unreachable
}
