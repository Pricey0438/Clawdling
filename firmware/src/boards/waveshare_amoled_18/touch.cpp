#include "../../hal/touch_hal.h"
#include "../../hal/i2c_health.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

// Minimal FT3168 reader (FocalTech standard register layout). Avoids
// vendoring Waveshare's GPLv3 Arduino_DriveBus library.
//   reg 0x02:        low nibble = active finger count
//   reg 0x03 / 0x04: X1 high (low nibble) + X1 low
//   reg 0x05 / 0x06: Y1 high (low nibble) + Y1 low

static volatile bool     touch_data_ready = false;
static volatile bool     touch_pressed = false;
static volatile uint16_t touch_x = 0;
static volatile uint16_t touch_y = 0;

// P2-9: spinlock protecting the (pressed, x, y) tuple. Today touch_hal_read
// is the only mutator AND the only consumer (LVGL touch callback in main
// loop), so a strict race is impossible — but the ISR-vs-loop split makes
// adding a second reader trivial-to-misuse. Pattern mirrors ble.cpp:rx_mux.
// The ISR itself never takes this lock: it only flips the volatile
// touch_data_ready bool (single-byte atomic on ESP32-S3). Mutations happen
// inside ft3168_read_into_shared_state — each release/error path takes the
// lock so a future second reader can never see a torn tuple.
static portMUX_TYPE touch_mux = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR touch_isr(void) {
    touch_data_ready = true;
}

static void ft3168_read_into_shared_state(void) {
    // I2C read happens outside the lock — Wire transactions can take
    // hundreds of µs and the lock should be held only long enough to
    // commit the new tuple coherently.
    Wire.beginTransmission(FT3168_ADDR);
    Wire.write(0x02);
    if (Wire.endTransmission(false) != 0) {
        i2c_health_report_fail();
        portENTER_CRITICAL(&touch_mux);
        touch_pressed = false;
        touch_x = 0;
        touch_y = 0;
        portEXIT_CRITICAL(&touch_mux);
        return;
    }
    if (Wire.requestFrom(FT3168_ADDR, (uint8_t)5) != 5) {
        i2c_health_report_fail();
        portENTER_CRITICAL(&touch_mux);
        touch_pressed = false;
        touch_x = 0;
        touch_y = 0;
        portEXIT_CRITICAL(&touch_mux);
        return;
    }
    i2c_health_report_ok();
    uint8_t fingers = Wire.read() & 0x0F;
    uint8_t xH = Wire.read();
    uint8_t xL = Wire.read();
    uint8_t yH = Wire.read();
    uint8_t yL = Wire.read();
    if (fingers == 0 || fingers > 5) {
        portENTER_CRITICAL(&touch_mux);
        touch_pressed = false;
        touch_x = 0;
        touch_y = 0;
        portEXIT_CRITICAL(&touch_mux);
        return;
    }
    portENTER_CRITICAL(&touch_mux);
    // Clear coords on every release/error path so LVGL never sees stale
    // (x,y) paired with pressed=false — that would fire ghost taps.
    touch_x = ((uint16_t)(xH & 0x0F) << 8) | xL;
    touch_y = ((uint16_t)(yH & 0x0F) << 8) | yL;
    touch_pressed = true;
    portEXIT_CRITICAL(&touch_mux);
}

void touch_hal_init(void) {
    // Power-mode register 0xA5 = 0x00: active scanning.
    Wire.beginTransmission(FT3168_ADDR);
    Wire.write(0xA5);
    Wire.write(0x00);
    Wire.endTransmission();

    // Verify device ID register 0xA0 (FT3168 reports 0x03 but Waveshare's
    // panel sometimes returns 0x86 — log but don't fail).
    Wire.beginTransmission(FT3168_ADDR);
    Wire.write(0xA0);
    if (Wire.endTransmission(false) == 0 && Wire.requestFrom(FT3168_ADDR, (uint8_t)1) == 1) {
        Serial.printf("FT3168 ID=0x%02X\n", Wire.read());
    } else {
        Serial.println("FT3168 ID read failed");
    }

    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(TP_INT, touch_isr, FALLING);
    Serial.println("FT3168 attached on INT pin");
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    if (touch_data_ready) {
        touch_data_ready = false;
        ft3168_read_into_shared_state();   // takes touch_mux around the writes
    }
    // Snapshot the tuple under the lock so a future second reader can't see
    // a torn (pressed=true, x=stale) combination.
    portENTER_CRITICAL(&touch_mux);
    bool     p_snap = touch_pressed;
    uint16_t x_snap = touch_x;
    uint16_t y_snap = touch_y;
    portEXIT_CRITICAL(&touch_mux);
    *x = x_snap;
    *y = y_snap;
    *pressed = p_snap;
}
