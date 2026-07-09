#include <Arduino.h>
#include "modem_urc.h"
#include "serial_comm.h" // extern HardwareSerial ss
#include "mqtt_lte.h"    // mqttLte_feedUrc(), mqttLte_feedMessage()
#include "sms_handler.h" // smsHandler_feedSms()

// ============================================================================
//  ÉTAT INTERNE
// ============================================================================

// Machine à états pour les URC multi-lignes
enum UrcState
{
    URC_IDLE,    // en attente d'un URC connu
    URC_MQTT_RX, // capture +CMQTTRXSTART ... +CMQTTRXEND
    URC_SMS_BODY // capture du corps SMS après +CMT: header
};

static UrcState urcState = URC_IDLE;
static uint32_t stateEnteredMs = 0; // pour timeout anti-blocage

// Buffer d'accumulation ligne par ligne
static String lineBuffer = "";

// Contexte capture MQTT RX (multi-lignes)
static String mqttRxTopic;
static String mqttRxPayload;
static bool mqttInTopic = false;
static bool mqttInPayload = false;

// Contexte capture SMS (2 lignes : header +CMT: puis body)
static String smsSender;

// Timeout : si on reste bloqué dans un état multi-lignes (modem glitch)
static const uint32_t URC_MULTILINE_TIMEOUT_MS = 8000;

// ============================================================================
//  TRAITEMENT D'UNE LIGNE COMPLÈTE
// ============================================================================
static void processLine(const String &line)
{
    switch (urcState)
    {

    // ── IDLE : on cherche un préfixe URC connu ─────────────────────────────
    case URC_IDLE:
        if (line.startsWith("+CMQTTCONNLOST"))
        {
            mqttLte_feedUrc(line);
        }
        else if (line.startsWith("+CMQTTNONET"))
        {
            mqttLte_feedUrc(line);
        }
        else if (line.startsWith("+CMQTTRXSTART"))
        {
            // Début d'un message MQTT entrant — séquence multi-lignes
            urcState = URC_MQTT_RX;
            stateEnteredMs = millis();
            mqttRxTopic = "";
            mqttRxPayload = "";
            mqttInTopic = false;
            mqttInPayload = false;
        }
        else if (line.startsWith("+CMT:"))
        {
            // Header SMS — extraire l'expéditeur, attendre le body
            int q1 = line.indexOf('"');
            int q2 = (q1 != -1) ? line.indexOf('"', q1 + 1) : -1;
            if (q1 != -1 && q2 != -1)
            {
                smsSender = line.substring(q1 + 1, q2);
                urcState = URC_SMS_BODY;
                stateEnteredMs = millis();
            }
            else
            {
                Serial.printf("[URC] +CMT: malformed header: %s\n", line.c_str());
            }
        }
        // Tout le reste (OK, ERROR, réponses AT non-URC) est ignoré ici.
        // Les commandes AT synchrones (SentMessage etc.) les consomment
        // directement depuis ss avant qu'on arrive ici.
        break;

    // ── MQTT RX : capture topic + payload ──────────────────────────────────
    case URC_MQTT_RX:
        if (line.startsWith("+CMQTTRXTOPIC"))
        {
            mqttInTopic = true;
            mqttInPayload = false;
        }
        else if (line.startsWith("+CMQTTRXPAYLOAD"))
        {
            mqttInTopic = false;
            mqttInPayload = true;
        }
        else if (line.startsWith("+CMQTTRXEND"))
        {
            // Séquence terminée — dispatcher le message
            urcState = URC_IDLE;
            if (mqttRxTopic.length() > 0)
            {
                mqttLte_feedMessage(mqttRxTopic, mqttRxPayload);
            }
        }
        else
        {
            // Ligne de données (topic ou payload)
            if (mqttInTopic)
                mqttRxTopic += line;
            if (mqttInPayload)
                mqttRxPayload += line;
        }
        break;

    // ── SMS BODY : la ligne suivante est le corps du SMS ───────────────────
    case URC_SMS_BODY:
        smsHandler_feedSms(smsSender, line);
        urcState = URC_IDLE;
        break;
    }
}

// ============================================================================
//  API PUBLIQUE
// ============================================================================
void modemUrc_init()
{
    lineBuffer = "";
    urcState = URC_IDLE;
    Serial.println("[URC] Modem URC dispatcher initialized");
}

void modemUrc_tick()
{
    if (!modemMutex_tryTake())
        // Serial.println("[URC] modemUrc_tick: cannot take mutex, skipping this tick");
        return;

    // 1) Lire tous les octets disponibles dans le buffer
    while (ss.available())
    {
        char c = (char)ss.read();
        lineBuffer += c;
    }

    // 2) Traiter les lignes complètes (terminées par '\n')
    int nlIdx;
    while ((nlIdx = lineBuffer.indexOf('\n')) != -1)
    {
        String line = lineBuffer.substring(0, nlIdx);
        line.trim();
        lineBuffer = lineBuffer.substring(nlIdx + 1);

        if (line.length() == 0)
            continue;

        processLine(line);
    }

    // 3) Timeout anti-blocage : si on est dans un état multi-lignes
    //    depuis trop longtemps, on revient à IDLE
    if (urcState != URC_IDLE &&
        (millis() - stateEnteredMs) > URC_MULTILINE_TIMEOUT_MS)
    {
        Serial.printf("[URC] Timeout in state %d — resetting to IDLE\n",
                      (int)urcState);
        urcState = URC_IDLE;
    }

    // 4) Éviter une croissance mémoire infinie du buffer résiduel
    if (lineBuffer.length() > 1024)
    {
        lineBuffer = lineBuffer.substring(lineBuffer.length() - 256);
    }
    // liberer le mutex
    modemMutex_give();
}