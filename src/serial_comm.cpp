#include <Arduino.h>
#include <HardwareSerial.h>
#include "serial_comm.h"
#include "config.h"

HardwareSerial ss(1); // UART1 → SIM7670G (pins définis dans config.h, begin() fait dans setupLTE())

// ============================================================================
//  COMMUNICATION SÉRIE AVEC LE SIM7670G
// ============================================================================

void SentSerial(const char *p_char) {
    ss.println(p_char);
}

bool SentMessage(const char *p_char, unsigned long timeout) {
    while (ss.available()) ss.read();

    ss.println(p_char);

    unsigned long start = millis();
    String resp = "";

    while (millis() - start < timeout) {
        while (ss.available()) resp += (char)ss.read();
        if (resp.indexOf("OK")    != -1) return true;
        if (resp.indexOf("ERROR") != -1) return false;
        delay(10);
    }
    return false;
}

String SentMessageResponse(const char *p_char, unsigned long timeout) {
    while (ss.available()) ss.read();

    ss.println(p_char);

    unsigned long start = millis();
    String resp = "";

    while (millis() - start < timeout) {
        while (ss.available()) resp += (char)ss.read();
        if (resp.indexOf("OK") != -1 || resp.indexOf("ERROR") != -1) break;
        delay(10);
    }
    return resp;
}

String waitForResponse(const String& expected, unsigned long timeout) {
    unsigned long start = millis();
    String resp = "";

    while (millis() - start < timeout) {
        while (ss.available()) {
            resp += (char)ss.read();
        }
        if (resp.indexOf(expected) != -1) break;
        delay(100);
    }
    return resp;
}
// ============================================================================
//  ENVOI AVEC PROMPT '>' (pour AT+CMQTTTOPIC / AT+CMQTTPAYLOAD)
// ============================================================================
// Séquence en 2 temps :
//   1) on envoie la commande (ex: AT+CMQTTTOPIC=0,35)
//   2) le modem répond '>' → on envoie alors les données (sans \r\n final)
//   3) le modem confirme par OK
bool SentPrompt(const char* cmd, const char* data, unsigned long timeout) {
    // Vider le buffer de réception
    while (ss.available()) ss.read();

    // 1) Envoyer la commande
    ss.println(cmd);

    // 2) Attendre le prompt '>'
    unsigned long start = millis();
    bool gotPrompt = false;
    String resp = "";

    while (millis() - start < timeout) {
        while (ss.available()) {
            char c = (char)ss.read();
            resp += c;
            if (c == '>') {
                gotPrompt = true;
                break;
            }
        }
        if (gotPrompt) break;
        if (resp.indexOf("ERROR") != -1) {
            Serial.printf("[AT] SentPrompt: ERROR before prompt: %s\n", resp.c_str());
            return false;
        }
        delay(5);
    }

    if (!gotPrompt) {
        Serial.printf("[AT] SentPrompt: no '>' prompt (resp=%s)\n", resp.c_str());
        return false;
    }

    // 3) Envoyer les données SANS \r\n (le modem compte les octets exacts)
    ss.print(data);

    // 4) Attendre le OK final
    start = millis();
    resp = "";
    while (millis() - start < timeout) {
        while (ss.available()) resp += (char)ss.read();
        if (resp.indexOf("OK")    != -1) return true;
        if (resp.indexOf("ERROR") != -1) {
            Serial.printf("[AT] SentPrompt: ERROR after data: %s\n", resp.c_str());
            return false;
        }
        delay(5);
    }

    Serial.println("[AT] SentPrompt: timeout waiting OK after data");
    return false;
}
// ============================================================================
//  ENVOI ASYNCHRONE (attente d'un URC spécifique)
// ============================================================================
// Pour les commandes qui répondent OK puis l'URC réel plus tard :
//   AT+CMQTTSTART   → OK puis +CMQTTSTART: 0
//   AT+CMQTTCONNECT → OK puis +CMQTTCONNECT: 0,0
//   AT+CMQTTPUB     → OK puis +CMQTTPUB: 0,0
//   AT+NETOPEN      → OK puis +NETOPEN: 0
String SentMessageAsync(const char* cmd, const char* expectedURC,
                        unsigned long timeout) {
    // Vider le buffer de réception
    delay(50);
    while (ss.available()) ss.read();

    ss.println(cmd);

    unsigned long start = millis();
    String resp = "";
    while (millis() - start < timeout) {
        while (ss.available()) resp += (char)ss.read();
        if (resp.indexOf(expectedURC)  != -1) break;
        if (resp.indexOf("+CME ERROR") != -1) break;
        delay(50);
    }
    return resp;
}
// ============================================================================
//  MUTEX UART MODEM
// ============================================================================
static SemaphoreHandle_t modemMtx = NULL;

void modemMutex_init()
{
    modemMtx = xSemaphoreCreateMutex();
    configASSERT(modemMtx != NULL);
    Serial.println("[AT] Modem UART mutex created");
}

bool modemMutex_take(uint32_t timeoutMs)
{
    if (modemMtx == NULL) return false;
    return (xSemaphoreTake(modemMtx, pdMS_TO_TICKS(timeoutMs)) == pdTRUE);
}

bool modemMutex_tryTake()
{
    if (modemMtx == NULL) return false;
    return (xSemaphoreTake(modemMtx, 0) == pdTRUE);
}

void modemMutex_give()
{
    if (modemMtx != NULL) {
        xSemaphoreGive(modemMtx);
    }
}