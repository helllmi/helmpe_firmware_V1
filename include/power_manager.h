#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include <Arduino.h>
#include <esp_task_wdt.h>
#include <esp_pm.h>
#include "config.h"


void power_init();

void power_setcpufreq(uint32_t freqMHz);

void power_enterstandby();

void power_enteraction();

uint32_t power_getcpufreq();
// Light sleep de courte durée (jusqu'à `durationMs` ms ou réveil sur événement).
// Réveils possibles :
//   - Timer : expiration du delay
//   - GPIO  : bouton SOS (configuré comme source de réveil)
//   - UART  : données arrivant du modem (URC MQTT)
// La RAM est conservée. La loop reprend exactement où elle dormait.
void power_lightSleep(uint32_t durationMs);
#endif