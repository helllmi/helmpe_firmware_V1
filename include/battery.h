#ifndef BATTERY_H
#define BATTERY_H

#include <Arduino.h>

#define MAX17048_ADDR     0x36
#define MAX17048_SOC_REG  0x04

float readBattery();

#endif
