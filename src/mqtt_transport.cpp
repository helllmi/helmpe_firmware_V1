#include <Arduino.h>
#include "mqtt_transport.h"
#include "config.h"
#include <WiFi.h>
#include <ArduinoJson.h>

#include "mqtt_client.h" // WiFi (existant) — donne accès aux topics extern
#include "mqtt_lte.h"    // LTE
#include "boot_mode.h"
#include "wifi_creds.h"
#include "jwt_storage.h"





static MqttRxCallback userCallback = nullptr;

static int consecutiveLteFailures = 0;
static const int MAX_LTE_FAILURES_BEFORE_FALLBACK = 3;

// ============================================================================
//  HELPER : décide si on est en WiFi (fallback) ou LTE (primaire)
// ============================================================================
static bool usingWifi()
{
    return bootMode_isForceWifi() && WiFi.status() == WL_CONNECTED;
}

bool mqttTransport_isWifi()
{
    return usingWifi();
}

// ============================================================================
//  CALLBACK TOKEN — partagé entre WiFi (PubSubClient) et LTE (URC)
// ============================================================================
// En WiFi, le callback token est déjà géré dans mqtt_client.cpp::onMqttMessage.
// En LTE, il faut router nous-mêmes ici car mqtt_lte.cpp est générique
// (il ne connaît pas la sémantique des topics).
static void handleLteTokenMessage(const String &payload)
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err)
    {
        Serial.printf("[MQTT-TRANSPORT] Token parse error: %s\n", err.c_str());
        return;
    }

    const char *newToken = doc["token"];
    if (newToken && strlen(newToken) > 0)
    {
        Serial.printf("[MQTT-TRANSPORT] Token received via LTE (%u chars)\n", strlen(newToken));
        if (jwtStorage_save(String(newToken)))
        {
            Serial.println("[MQTT-TRANSPORT] Token saved, rebooting in 3s");
            delay(3000);
            ESP.restart();
        }
    }
    else
    {
        Serial.println("[MQTT-TRANSPORT] Token message missing 'token' field");
    }
}

// ============================================================================
//  CALLBACK INTERMÉDIAIRE pour LTE
// ============================================================================
static void onLteMessage(const String &topic, const String &payload)
{
    // Interception du topic token (sémantique connue ici, pas dans mqtt_lte.cpp)
    if (topic == topicToken)
    {
        handleLteTokenMessage(payload);
        return;
    }
    if (userCallback)
        userCallback(topic, payload);
}

// ============================================================================
//  INIT
// ============================================================================
bool mqttTransport_begin(const char *broker, uint16_t port, const char *clientId)
{
    // mqttBegin construit les topics globaux (utilisés aussi par LTE)
    mqttBegin(broker, port, clientId);

    bool useWifi = usingWifi();
    Serial.printf("[MQTT-TRANSPORT] Effective transport: %s\n",
                  useWifi ? "WIFI (fallback)" : "LTE (primary)");

    if (useWifi)
    {
        // ── Mode WiFi (fallback) ──────────────────────────────────────
        Serial.println("[MQTT-TRANSPORT] Using WiFi for MQTT");
        return connectMqtt();
    }
    else
    {
        // ── Mode LTE (normal) ─────────────────────────────────────────
        bool ok = mqttLte_start(broker, port, clientId);
        if (ok)
        {
            mqttLte_onMessage(onLteMessage);
            mqttTransport_subscribeAll();
            consecutiveLteFailures = 0;
        }
        else
        {
            consecutiveLteFailures++;
        }
        return ok;
    }
}

// ============================================================================
//  ÉTAT
// ============================================================================
bool mqttTransport_isConnected()
{
    if (usingWifi())
        return isMqttConnected();
    return mqttLte_isConnected();
}

// ============================================================================
//  RECONNEXION
// ============================================================================
bool mqttTransport_reconnect()
{
   /* if (suspended)
        return false;**/
    if (usingWifi())
    {
        // En WiFi : juste tenter une reconnexion MQTT
        return connectMqtt();
    }

    // En LTE : tentative de reconnexion + comptage des échecs
    Serial.println("[MQTT-TRANSPORT] LTE reconnect attempt");

    if (mqttLte_needsRestart())
    {
        mqttLte_stop();
    }

    bool ok = mqttLte_start(MQTT_BROKER_HOST, MQTT_BROKER_PORT, DEVICE_ID);
    if (ok)
    {
        mqttLte_onMessage(onLteMessage);
        mqttTransport_subscribeAll();
        consecutiveLteFailures = 0;
        return true;
    }

    // Échec : incrémenter le compteur
    consecutiveLteFailures++;
    Serial.printf("[MQTT-TRANSPORT] LTE failure %d/%d\n",
                  consecutiveLteFailures, MAX_LTE_FAILURES_BEFORE_FALLBACK);

    // Si seuil atteint et creds WiFi dispo → bascule via reboot
    if (consecutiveLteFailures >= MAX_LTE_FAILURES_BEFORE_FALLBACK)
    {
        if (wifiCreds_hasSsid())
        {
            Serial.println("[MQTT-TRANSPORT] *** LTE FAILED TOO MANY TIMES ***");
            Serial.println("[MQTT-TRANSPORT] Falling back to WiFi (rebooting)");
            bootMode_setForceWifi(true);
            delay(500);
            ESP.restart();
        }
        else
        {
            Serial.println("[MQTT-TRANSPORT] LTE failed but no WiFi creds saved");
            Serial.println("[MQTT-TRANSPORT] Use captive portal to set WiFi fallback");
            consecutiveLteFailures = 0;
        }
    }

    return false;
}

// ============================================================================
//  LOOP
// ============================================================================
void mqttTransport_loop()
{
    //if (suspended)
        //return; // ← ajouter ici
    if (usingWifi())
        mqttLoopTick();
    else
        mqttLte_loop();
}

// ============================================================================
//  PUBLICATIONS
// ============================================================================
bool mqttTransport_publishAlert(const String &payload)
{
    if (usingWifi())
        return mqttPublishAlert(payload);
    return mqttLte_publish(topicAlert.c_str(), payload.c_str(), 1, false);
}

bool mqttTransport_publishTelemetry(const String &payload)
{
    if (usingWifi())
        return mqttPublishTelemetry(payload);
    return mqttLte_publish(topicTelemetry.c_str(), payload.c_str(), 0, false);
}

bool mqttTransport_publishState(const String &payload)
{
    if (usingWifi())
        return mqttPublishState(payload);
    return mqttLte_publish(topicState.c_str(), payload.c_str(), 0, true);
}

bool mqttTransport_publishOtaStatus(const String &payload)
{
    if (usingWifi())
        return mqttPublishOtaStatus(payload.c_str());
    return mqttLte_publish(topicOtaStatus.c_str(), payload.c_str(), 1, false);
}

// ============================================================================
//  SOS — route LTE/WiFi
// ============================================================================
bool mqttTransport_publishSos(const String &recordId, double latitude, double longitude,
                              const char *isoTimestamp, float batteryLevel, int rssi)
{
    if (usingWifi())
    {
        return mqttPublishSos(recordId, latitude, longitude, isoTimestamp, batteryLevel, rssi);
    }

    // LTE : construire le JSON nous-mêmes (mqttLte_publish prend un payload brut)
    JsonDocument doc;
    doc["device_id"] = DEVICE_ID;
    doc["latitude"] = latitude;
    doc["longitude"] = longitude;
    doc["alert_type"] = "sos";
    doc["timestamp"] = isoTimestamp;
    doc["record_id"] = recordId;
    doc["is_recording"] = true;
    doc["battery_level"] = batteryLevel;
    doc["rssi"] = rssi;

    char payload[512];
    serializeJson(doc, payload, sizeof(payload));

    Serial.printf("[MQTT-TRANSPORT] PUB sos via LTE on %s\n", topicSos.c_str());
    return mqttLte_publish(topicSos.c_str(), payload, 1, false);
}

// ============================================================================
//  ABONNEMENTS
// ============================================================================
bool mqttTransport_subscribeAll()
{
    if (usingWifi())
    {
        // WiFi : déjà géré par connectMqtt() dans mqtt_client.cpp
        return true;
    }
    // LTE : on s'abonne manuellement aux topics descendants
    bool ok = true;
    ok &= mqttLte_subscribe(topicOtaNotify.c_str(), 1);
    ok &= mqttLte_subscribe(topicToken.c_str(), 1);
    return ok;
}

// ============================================================================
//  CALLBACK
// ============================================================================
void mqttTransport_onMessage(MqttRxCallback cb)
{
    userCallback = cb;
    // LTE : branché à onLteMessage dans begin/reconnect
    // WiFi : PubSubClient utilise déjà mqtt.setCallback() dans mqtt_client.cpp
}

// ============================================================================
//  TYPE
// ============================================================================
const char *mqttTransport_typeName()
{
    return usingWifi() ? "WIFI" : "LTE";
}