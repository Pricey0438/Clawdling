#include "../../hal/touch_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <TouchDrvCSTXXX.hpp>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

static TouchDrvCST92xx touch;

static volatile bool     touch_data_ready = false;
static volatile bool     touch_pressed = false;
static volatile uint16_t touch_x = 0;
static volatile uint16_t touch_y = 0;

// P2-9: spinlock protecting the (pressed, x, y) tuple. Today touch_hal_read
// is the only mutator AND the only consumer (LVGL touch callback in main
// loop), so a strict race is impossible — but the ISR-vs-loop split makes
// adding a second reader trivial-to-misuse. Pattern mirrors ble.cpp:rx_mux.
// The ISR itself never takes this lock: it only flips the volatile
// touch_data_ready bool (single-byte atomic on ESP32-S3).
static portMUX_TYPE touch_mux = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR touch_isr(void) {
    touch_data_ready = true;
}

void touch_hal_init(void) {
    touch.setPins(TP_RST, TP_INT);
    if (!touch.begin(Wire, CST9220_ADDR, IIC_SDA, IIC_SCL)) {
        Serial.println("Touch init failed");
        return;
    }
    touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
    touch.setSwapXY(true);
    touch.setMirrorXY(true, false);
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(TP_INT, touch_isr, FALLING);
    Serial.println("Touch init OK");
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    if (touch_data_ready) {
        touch_data_ready = false;
        // I2C read happens outside the lock — touch.getPoint() can take
        // hundreds of µs and the lock should be held only long enough to
        // commit the new tuple coherently.
        int16_t tx[5], ty[5];
        uint8_t n = touch.getPoint(tx, ty, touch.getSupportTouchPoint());
        portENTER_CRITICAL(&touch_mux);
        if (n > 0) {
            touch_pressed = true;
            touch_x = (uint16_t)tx[0];
            touch_y = (uint16_t)ty[0];
        } else {
            // Clear coords on release so LVGL never sees a stale (x,y)
            // paired with pressed=false → would fire ghost taps.
            touch_pressed = false;
            touch_x = 0;
            touch_y = 0;
        }
        portEXIT_CRITICAL(&touch_mux);
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
