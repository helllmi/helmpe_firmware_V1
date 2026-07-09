#ifndef SMS_HANDLER_H
#define SMS_HANDLER_H

#include <Arduino.h>

// ============================================================================
//  SMS_HANDLER — commandes à distance via SIM7670G
// ============================================================================
//  Commandes supportées (numéro autorisé uniquement — voir SMS_AUTHORIZED_NUMBER) :
//
//    STATUS  → état FSM, batterie, transport, GPS valid, uptime
//    GPS     → latitude, longitude, altitude, vitesse
//    SIGNAL  → RSSI, BER, opérateur (AT+CSQ + AT+COPS)
//    RESET   → EVT_USER_RESET (retour STANDBY)
//    SOS     → EVT_SOS_TRIGGERED (déclenche ACTION)
//
//  Mode réception : URC push (AT+CNMI=2,2) — les SMS arrivent directement
//  sur UART sans polling de mémoire.
//  Mode texte : AT+CMGF=1
// ============================================================================

// Initialise le mode SMS du modem (AT+CMGF=1, AT+CNMI=2,2)
void smsHandler_init();

// ── Appelé par modem_urc.cpp quand un SMS complet est reçu (+CMT:) ────
void smsHandler_feedSms(const String& sender, const String& body);

#endif