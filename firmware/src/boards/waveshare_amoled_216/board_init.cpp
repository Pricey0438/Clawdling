#include "board.h"
#include "hal/i2c_health.h"
#include <Arduino.h>
#include <Wire.h>

// Bring up the shared I2C bus. AMOLED-2.16 has no IO expander, so this is
// all the early init needed before display/touch/power/imu HAL calls.
extern "C" void board_init(void) {
    Wire.begin(IIC_SDA, IIC_SCL);
    i2c_health_register(IIC_SDA, IIC_SCL);
}
