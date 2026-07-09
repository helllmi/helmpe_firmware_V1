#ifndef LTE_H
#define LTE_H

#include <Arduino.h>

struct SignalInfo
{
    int rssi = -1;
    int ber = -1;
};

// Initialise le contexte LTE (APN, PDP activation)
// À garder même si MQTT passe par Wi-Fi : utilisé pour signal/horodatage
bool setupLTE();

// Horodatage réseau (AT+CCLK?) → format ISO 8601
String getNetworkTime();

// Force du signal LTE (AT+CSQ)
SignalInfo getSignalInfo();

#endif
