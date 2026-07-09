#ifndef SERIAL_COMM_H
#define SERIAL_COMM_H

#include <Arduino.h>
#include <HardwareSerial.h>

extern HardwareSerial ss;

// Fonctions de communication série avec le SIM7670G
void SentSerial(const char *p_char);
bool SentMessage(const char *p_char, unsigned long timeout = 2000);
String SentMessageResponse(const char *p_char, unsigned long timeout = 3000);
String waitForResponse(const String &expected, unsigned long timeout);
// Envoie une commande qui attend un prompt '>', puis envoie les données.
// Utilisé pour AT+CMQTTTOPIC et AT+CMQTTPAYLOAD du MQTT natif.
// Retourne true si la séquence se termine par OK.
bool SentPrompt(const char *cmd, const char *data, unsigned long timeout = 5000);
String SentMessageAsync(const char *cmd, const char *expectedURC,
                        unsigned long timeout = 15000);
// ── Mutex UART modem (protège ss contre accès concurrent) ──────────────
void modemMutex_init();
bool modemMutex_take(uint32_t timeoutMs = 5000);
bool modemMutex_tryTake();
void modemMutex_give();
#endif
