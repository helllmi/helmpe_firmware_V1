#include <Arduino.h>
#include <Wire.h>
#include "battery.h"

// ============================================================================
//  LECTURE BATTERIE via MAX17048 (I2C)
// ============================================================================

float readBattery() {
    Wire.beginTransmission(MAX17048_ADDR);
    Wire.write(MAX17048_SOC_REG);
    Wire.endTransmission(false);

    Wire.requestFrom(MAX17048_ADDR, 2);

    if (Wire.available() < 2) {
        Serial.println("Battery sensor not found!");
        return -1.0;
    }

    uint8_t msb = Wire.read();
    uint8_t lsb = Wire.read();

    // MSB = pourcentage entier, LSB = fraction (1/256 %)
    float soc = msb + (lsb / 256.0);

    if (soc > 100.0) soc = 100.0;
    if (soc < 0.0)   soc = 0.0;

    return soc;
}
