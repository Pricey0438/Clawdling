#include "i2c_health.h"
#include <Arduino.h>
#include <Wire.h>

static uint8_t  s_sda      = 0;
static uint8_t  s_scl      = 0;
static bool     s_armed    = false;     // true once register() ran
static uint16_t s_fails    = 0;         // consecutive failure count
static uint32_t s_total_recoveries = 0;
static bool     s_recovery_pending = false;

void i2c_health_register(uint8_t sda, uint8_t scl) {
    s_sda   = sda;
    s_scl   = scl;
    s_armed = true;
    s_fails = 0;
}

void i2c_health_report_ok(void) {
    s_fails = 0;
}

void i2c_health_report_fail(void) {
    if (s_fails < 0xFFFF) s_fails++;
    if (s_fails >= I2C_HEALTH_FAIL_THRESHOLD) s_recovery_pending = true;
}

uint32_t i2c_health_recovery_count(void) {
    return s_total_recoveries;
}

void i2c_health_tick(void) {
    if (!s_armed || !s_recovery_pending) return;

    // Drain pending Wire driver state and bring the master back up. The
    // slaves (FT3168/CST9220 touch, AXP2101 PMU, QMI8658 IMU, XCA9554 IO
    // expander) keep their own state through this — only the ESP32-side
    // master driver gets re-initialised.
    Serial.printf("i2c_health: recovering after %u consecutive failures\n",
                  (unsigned)s_fails);
    Wire.end();
    delay(5);
    Wire.begin((int)s_sda, (int)s_scl);
    delay(5);

    s_fails             = 0;
    s_recovery_pending  = false;
    s_total_recoveries++;
}
