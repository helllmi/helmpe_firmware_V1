#include <Arduino.h>
#include "sms_handler.h"
#include "serial_comm.h"
#include "config.h"
#include "state_machine.h"
#include "battery.h"
#include "gps.h"
#include "mqtt_transport.h"

// ============================================================================
//  HELPERS INTERNES
// ============================================================================

// Envoie un SMS au numéro autorisé
static void smsSend(const String& message)
{
    char cmd[40];
    snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"", SMS_AUTHORIZED_NUMBER);

    ss.println(cmd);

    unsigned long start = millis();
    bool gotPrompt = false;
    while (millis() - start < 5000) {
        if (ss.available()) {
            char c = ss.read();
            if (c == '>') { gotPrompt = true; break; }
        }
    }

    if (!gotPrompt) {
        Serial.println("[SMS] No prompt received for CMGS");
        ss.println((char)27);
        return;
    }

    ss.print(message);
    ss.write(0x1A);

    start = millis();
    String resp = "";
    while (millis() - start < 10000) {
        while (ss.available()) resp += (char)ss.read();
        if (resp.indexOf("OK")    != -1 ||
            resp.indexOf("ERROR") != -1) break;
        delay(10);
    }

    if (resp.indexOf("OK") != -1) {
        Serial.printf("[SMS] Sent OK: %s\n", message.c_str());
    } else {
        Serial.printf("[SMS] Send failed: %s\n", resp.c_str());
    }
}

// ============================================================================
//  HANDLERS PAR COMMANDE
// ============================================================================

static void handleStatus()
{
    const char* stateName = stateMachine_stateName(stateMachine_getState());
    float       battery   = readBattery();
    const char* transport = mqttTransport_typeName();
    bool        gpsValid  = currentGPS.valid;
    uint32_t    uptime    = millis() / 1000;

    char resp[200];
    snprintf(resp, sizeof(resp),
             "STATUS\n"
             "State: %s\n"
             "Battery: %d%%\n"
             "Transport: %s\n"
             "MQTT: %s\n"
             "GPS: %s\n"
             "Uptime: %us",
             stateName,
             (int)battery,
             transport,
             mqttTransport_isConnected() ? "OK" : "DOWN",
             gpsValid ? "VALID" : "NO FIX",
             uptime);

    smsSend(String(resp));
}

static void handleGps()
{
    if (!currentGPS.valid) {
        smsSend("GPS: NO FIX");
        return;
    }

    char resp[160];
    snprintf(resp, sizeof(resp),
             "GPS\n"
             "Lat: %.6f\n"
             "Lon: %.6f\n"
             "Alt: %.1fm\n"
             "Speed: %.1fkm/h",
             currentGPS.latitude,
             currentGPS.longitude,
             currentGPS.altitude,
             currentGPS.speed);

    smsSend(String(resp));
}

static void handleSignal()
{
    String csq = SentMessageResponse("AT+CSQ", 3000);
    String cops = SentMessageResponse("AT+COPS?", 3000);

    int rssi = -1, ber = -1;
    int csqIdx = csq.indexOf("+CSQ: ");
    if (csqIdx != -1) {
        String csqVal = csq.substring(csqIdx + 6);
        rssi = csqVal.toInt();
        int commaIdx = csqVal.indexOf(',');
        if (commaIdx != -1) ber = csqVal.substring(commaIdx + 1).toInt();
    }

    String oper = "UNKNOWN";
    int q1 = cops.indexOf('"');
    if (q1 != -1) {
        int q2 = cops.indexOf('"', q1 + 1);
        if (q2 != -1) oper = cops.substring(q1 + 1, q2);
    }

    int rssiDbm = (rssi >= 0 && rssi <= 31) ? (-113 + 2 * rssi) : 0;

    char resp[120];
    snprintf(resp, sizeof(resp),
             "SIGNAL\n"
             "RSSI: %ddBm\n"
             "BER: %d\n"
             "Operator: %s",
             rssiDbm, ber, oper.c_str());

    smsSend(String(resp));
}

static void handleReset()
{
    DeviceState current = stateMachine_getState();
    if (current == STATE_ACTION) {
        stateMachine_dispatch(EVT_USER_RESET);
        smsSend("CMD RESET: OK -> STANDBY");
        Serial.println("[SMS] CMD RESET executed");
    } else {
        smsSend("CMD RESET: already STANDBY");
        Serial.println("[SMS] CMD RESET: already STANDBY, ignored");
    }
}

static void handleSos()
{
    DeviceState current = stateMachine_getState();
    if (current == STATE_STANDBY) {
        stateMachine_dispatch(EVT_SOS_TRIGGERED);
        smsSend("CMD SOS: OK -> ACTION");
        Serial.println("[SMS] CMD SOS executed");
    } else {
        smsSend("CMD SOS: already ACTION");
        Serial.println("[SMS] CMD SOS: already ACTION, ignored");
    }
}

// ============================================================================
//  ROUTEUR DE COMMANDES
// ============================================================================
static void processCommand(const String& sender, const String& text)
{
    Serial.printf("[SMS] From: %s | Text: %s\n", sender.c_str(), text.c_str());

    if (sender.indexOf(SMS_AUTHORIZED_NUMBER) == -1) {
        Serial.printf("[SMS] UNAUTHORIZED number: %s — ignored\n", sender.c_str());
        return;
    }

    String cmd = text;
    cmd.trim();
    cmd.toUpperCase();

    if      (cmd == "STATUS") handleStatus();
    else if (cmd == "GPS")    handleGps();
    else if (cmd == "SIGNAL") handleSignal();
    else if (cmd == "RESET")  handleReset();
    else if (cmd == "SOS")    handleSos();
    else {
        Serial.printf("[SMS] Unknown command: %s\n", cmd.c_str());
        smsSend("Unknown cmd. Available: STATUS GPS SIGNAL RESET SOS");
    }
}

// ============================================================================
//  API PUBLIQUE
// ============================================================================

void smsHandler_init()
{
    Serial.println("[SMS] Initializing SMS handler...");

    SentMessage("AT+CMGF=1", 3000);
    delay(200);

    SentMessage("AT+CNMI=2,2,0,0,0", 3000);
    delay(200);

    Serial.println("[SMS] Handler ready (URC push mode)");
}


// ============================================================================
//  FEED SMS — appelé par modem_urc.cpp quand un +CMT: complet est reçu
// ============================================================================
void smsHandler_feedSms(const String &sender, const String &body)
{
    processCommand(sender, body);
}