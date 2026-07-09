#ifndef FALLBACKWIFI_H
#define FALLBACKWIFI_H

#include <Arduino.h>
#include <WiFi.h>

bool connectWiFi(uint32_t timeoutMs = 10000);

// Vérifie l'état Wi-Fi
bool isWiFiConnected();

#endif
