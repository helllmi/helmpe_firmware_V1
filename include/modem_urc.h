#ifndef MODEM_URC_H
#define MODEM_URC_H

#include <Arduino.h>

// ============================================================================
//  MODEM_URC — Lecteur unique du UART modem (UART1 / ss)
// ============================================================================
//  Résout la contention entre mqttLte_loop() et smsHandler_tick() qui
//  lisaient tous les deux ss en parallèle, causant des pertes de URC.
//
//  Tous les URC non-sollicités du SIM7670G passent par ici :
//    +CMQTTCONNLOST / +CMQTTNONET / +CMQTTRXSTART  → mqtt_lte
//    +CMT:                                          → sms_handler
//
//  Les commandes AT synchrones (SentMessage, SentPrompt, etc.) continuent
//  à lire ss directement — elles sont bloquantes et ne tournent pas en
//  même temps que modemUrc_tick() dans la même tâche (loop).
// ============================================================================

void modemUrc_init();

// À appeler dans loop(), AVANT mqttTransport_loop() et smsHandler_tick()
void modemUrc_tick();

#endif