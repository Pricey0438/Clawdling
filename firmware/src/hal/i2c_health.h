#pragma once
#include <stdint.h>

// Recover from an I2C master wedge (ESP_ERR_INVALID_STATE / repeated
// "i2c_master_transmit_receive failed" from Wire). The display keeps
// rendering over QSPI so the device LOOKS frozen even though the main
// loop is alive — touch/PMU/IMU just stop responding.
//
// Boards call i2c_health_register() once from board_init() right after
// Wire.begin() with the same pins. Per-loop code that talks I2C reports
// transaction outcomes via *_report_ok / *_report_fail. i2c_health_tick()
// (called from the main loop) trips a Wire.end()/Wire.begin() recovery
// when the consecutive-fail counter crosses I2C_HEALTH_FAIL_THRESHOLD.

#define I2C_HEALTH_FAIL_THRESHOLD 20   // ~1s of pure failure at 50ms touch poll

void i2c_health_register(uint8_t sda, uint8_t scl);

void i2c_health_report_ok(void);
void i2c_health_report_fail(void);

// Returns total recovery attempts since boot (for diagnostics).
uint32_t i2c_health_recovery_count(void);

// Call from main loop; cheap — no-op until threshold is hit.
void i2c_health_tick(void);
