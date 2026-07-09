#include <Arduino.h>
#include "lte.h"
#include "serial_comm.h"
#include "config.h"
#include "boot_mode.h"

// ============================================================================
//  CONFIGURATION LTE
// ============================================================================
// httpPostLTE() et sendAlertLTE() ont été retirés. Les alertes partent
// désormais par MQTT (cf. mqtt_client.cpp). La couche LTE est conservée
// pour :
//   - l'horodatage réseau (CCLK) utilisé dans le JSON d'alerte
//   - la mesure de signal (CSQ) intégrée à la télémétrie
//   - la migration future où MQTT passera par AT+CMQTT* du SIM7670G
//   - l'OTA en LTE via AT+HTTPDATA chunked (à venir)
// ============================================================================

String getNetworkTime()
{
    if (!modemMutex_tryTake())
    {
        Serial.println("[LTE] Modem busy, skipping getNetworkTime");
        return "";
    }

    String resp = SentMessageResponse("AT+CCLK?", 3000);
    // Réponse type : +CCLK: "26/04/16,13:32:45+04"
    int idx = resp.indexOf("+CCLK: \"");
    if (idx == -1)
    {
        modemMutex_give();
        return "";
    }

    String raw = resp.substring(idx + 8);
    raw = raw.substring(0, raw.indexOf("\""));

    // Vérifier que la réponse est valide (longueur minimale attendue)
    if (raw.length() < 17)
    {
        Serial.printf("[LTE] CCLK response too short: '%s'\n", raw.c_str());
        modemMutex_give();
        return "";
    }

    String yy  = raw.substring(0, 2);
    String mm  = raw.substring(3, 5);
    String dd  = raw.substring(6, 8);
    String hh  = raw.substring(9, 11);
    String min = raw.substring(12, 14);
    String sec = raw.substring(15, 17);

    // Vérifier que l'année est plausible
    int year = (2000 + yy.toInt());
    if (year < 2024 || year > 2100)
    {
        Serial.printf("[LTE] CCLK year invalid (%d), returning empty\n", year);
        modemMutex_give();
        return "";
    }

    modemMutex_give();
    return "20" + yy + "-" + mm + "-" + dd + "T" + hh + ":" + min + ":" + sec + ".000Z";
}

bool setupLTE()
{
    Serial.println("[LTE] Powering on modem (LilyGo sequence)...");

    // 1) Reset modem
    pinMode(MODEM_RESET_PIN, OUTPUT);
    digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);
    delay(100);
    digitalWrite(MODEM_RESET_PIN, MODEM_RESET_LEVEL);
    delay(MODEM_RESET_PULSE_WIDTH_MS);
    digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);

    // 2) DTR LOW = modem actif
    pinMode(MODEM_DTR_PIN, OUTPUT);
    digitalWrite(MODEM_DTR_PIN, LOW);

    // 3) Pulse PWRKEY
    pinMode(MODEM_PWRKEY_PIN, OUTPUT);
    digitalWrite(MODEM_PWRKEY_PIN, LOW);
    delay(100);
    digitalWrite(MODEM_PWRKEY_PIN, HIGH);
    delay(MODEM_POWERON_PULSE_WIDTH_MS);
    digitalWrite(MODEM_PWRKEY_PIN, LOW);

    // 4) Démarrer UART vers le modem
    ss.begin(230400, SERIAL_8N1, PIN_SIM_RX, PIN_SIM_TX);
    Serial.printf("[LTE] SerialAT started RX=%d TX=%d\n", PIN_SIM_RX, PIN_SIM_TX);

    // 5) Attendre réponse AT (~10s max)
    Serial.print("[LTE] Waiting for modem AT response");
    bool modemReady = false;
    for (int i = 0; i < 20; i++)
    {
        if (SentMessage("AT", 500))
        {
            modemReady = true;
            break;
        }
        Serial.print(".");
        delay(500);
    }
    Serial.println();
    if (!modemReady)
    {
        Serial.println("[LTE] Re-pulsing PWRKEY...");
        digitalWrite(MODEM_PWRKEY_PIN, HIGH);
        delay(MODEM_POWERON_PULSE_WIDTH_MS);
        digitalWrite(MODEM_PWRKEY_PIN, LOW);
        delay(3000);
        modemReady = SentMessage("AT", 2000);
    }
    Serial.println(modemReady ? "[LTE] Modem OK" : "[LTE] Modem not responding!");

    Serial.println("Setting up LTE...");

    String cereg = SentMessageResponse("AT+CEREG?", 3000);
    Serial.println("CEREG: " + cereg);

    String apnCmd = String("AT+CGDCONT=1,\"IP\",\"") + LTE_APN + "\"";
    SentMessage(apnCmd.c_str(), 3000);
    delay(500);

    if (!SentMessage("AT+CGACT=1,1", 10000))
    {
        Serial.println("PDP activation failed, retrying...");
        delay(2000);
        SentMessage("AT+CGACT=1,1", 10000);
    }

    String ip = SentMessageResponse("AT+CGPADDR=1", 3000);

    Serial.println("IP: " + ip);
    Serial.println("[LTE] Opening network stack (NETOPEN)...");

    // Vérifier si déjà ouvert
    String netCheck = SentMessageResponse("AT+NETOPEN?", 3000);
    if (netCheck.indexOf("+NETOPEN: 1") != -1)
    {
        Serial.println("[LTE] Network stack already open");
    }
    else
    {
        SentMessage("AT+NETOPEN", 15000);
        delay(2000);

        netCheck = SentMessageResponse("AT+NETOPEN?", 3000);
        if (netCheck.indexOf("+NETOPEN: 1") == -1)
        {
            Serial.println("[LTE] WARNING: NETOPEN unconfirmed, MQTT-LTE may fail");
        }
        else
        {
            Serial.println("[LTE] Network stack opened");
            if (bootMode_isForceWifi())
            {
                Serial.println("[LTE] LTE working — clearing WiFi fallback flag");
                bootMode_setForceWifi(false);
            }
        }
    }

    // LTE réseau opérationnel  effacer le flag WiFi fallback

    delay(1000);
    Serial.println("[LTE] Enabling modem sleep (CSCLK=1)...");
   // SentMessage("AT+CSCLK=1", 3000);
    delay(500);

    Serial.println("LTE Ready");
    return true;
}

SignalInfo getSignalInfo()
{
    SignalInfo info;

    if (!modemMutex_tryTake())
    {
        Serial.println("[LTE] Modem busy, skipping getSignalInfo");
        return info;
    }

    String resp = SentMessageResponse("AT+CSQ", 3000);
    int idx = resp.indexOf("+CSQ: ");
    if (idx == -1)
    {
        modemMutex_give();
        return info;
    }

    String data = resp.substring(idx + 6);
    info.rssi = data.substring(0, data.indexOf(",")).toInt();
    info.ber = data.substring(data.indexOf(",") + 1).toInt();

    Serial.printf("Signal RSSI: %d, BER: %d\n", info.rssi, info.ber);
    modemMutex_give();
    return info;
}
