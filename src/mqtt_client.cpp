#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "mqtt_client.h"
#include "ota.h"
#include "led.h"
#include "jwt_storage.h"
#include "config.h"
#include <SD.h>

// ============================================================================
//  ÉTAT GLOBAL
// ============================================================================
static WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// Topics (construits depuis DEVICE_ID au mqttBegin)
String topicAlert;
String topicTelemetry;
String topicOtaNotify;
String topicOtaStatus;
String topicLwt;
String topicState;
String topicSos;
String topicToken;

static String mqttBroker;
static uint16_t mqttPort;
static String deviceIdStr;

// ============================================================================
//  CALLBACK : réception token JWT (helpme/{id}/token)
// ============================================================================
// Payload attendu : {"token": "...", "expires_in_days": 365, "version": 1}
static void handleTokenMessage(byte *payload, unsigned int length)
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err) {
        Serial.printf("[MQTT] Token parse error: %s\n", err.c_str());
        return;
    }

    const char *newToken = doc["token"];
    if (newToken && strlen(newToken) > 0) {
        Serial.printf("[MQTT] Token received (%u chars), saving to NVS...\n", strlen(newToken));
        if (jwtStorage_save(String(newToken))) {
            Serial.println("[MQTT] Token updated successfully, rebooting in 3s");
            delay(3000);
            
            ESP.restart();
        }
    } else {
        Serial.println("[MQTT] Token message missing 'token' field");
    }
}

// ============================================================================
//  CALLBACK : réception d'une notification OTA
// ============================================================================
// Payload attendu :
//   {
//     "version": "1.1.0",
//     "url":     "http://192.168.x.x:5000/firmware/v1.1.0.bin",
//     "sha256":  "abcd...64 hex chars...",
//     "size":    945632
//   }
static void handleOtaMessage(byte *payload, unsigned int length)
{
    if (otaInProgress)
    {
        Serial.println("[MQTT] OTA already in progress, ignoring");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err)
    {
        Serial.printf("[MQTT] JSON parse error: %s\n", err.c_str());
        return;
    }

    String version = doc["version"].as<String>();
    String url = doc["url"].as<String>();
    String sha256 = doc["sha256"].as<String>();
    size_t size = doc["size"] | 0;

    Serial.printf("[MQTT] OTA notify: v%s, size=%u\n", version.c_str(), size);

    if (version == FIRMWARE_VERSION)
    {
        Serial.println("[MQTT] Already on this version, ignoring");
        return;
    }

    if (url.length() == 0 || sha256.length() != 64)
    {
        Serial.println("[MQTT] Invalid payload");
        return;
    }

    otaInProgress = true;
    performOTA(url, sha256, size);
    otaInProgress = false;
}

// ============================================================================
//  ROUTEUR DE MESSAGES ENTRANTS
// ============================================================================
static void onMqttMessage(char *topic, byte *payload, unsigned int length)
{
    Serial.printf("\n[MQTT] Message received on '%s' (%u bytes)\n", topic, length);

    String topicStr = String(topic);

    if (topicStr == topicOtaNotify) {
        handleOtaMessage(payload, length);
    } else if (topicStr == topicToken) {
        handleTokenMessage(payload, length);
    } else {
        Serial.println("[MQTT] Ignored (unknown topic)");
    }
}

// ============================================================================
//  INIT
// ============================================================================
void mqttBegin(const char *broker, uint16_t port, const char *deviceId)
{
    mqttBroker = String(broker);
    mqttPort = port;
    deviceIdStr = String(deviceId);

    topicAlert = "devices/" + deviceIdStr + "/alert";
    topicTelemetry = "devices/" + deviceIdStr + "/telemetry";
    topicOtaNotify = "devices/" + deviceIdStr + "/ota/notify";
    topicOtaStatus = "devices/" + deviceIdStr + "/ota/status";
    topicLwt = "devices/" + deviceIdStr + "/lwt";
    topicState = "devices/" + deviceIdStr + "/state";

    // Topics plateforme HEELPMEE (format helpme/%s/...)
    char buf[64];
    snprintf(buf, sizeof(buf), MQTT_TOPIC_SOS_FMT, deviceId);
    topicSos = String(buf);
    snprintf(buf, sizeof(buf), MQTT_TOPIC_TOKEN_FMT, deviceId);
    topicToken = String(buf);

    mqtt.setServer(mqttBroker.c_str(), mqttPort);
    mqtt.setCallback(onMqttMessage);
    mqtt.setBufferSize(2048); // assez pour le JSON d'alerte (~1500 octets)
    mqtt.setKeepAlive(30);    // 30s, compatible LTE PSM

    Serial.printf("[MQTT] Config: broker=%s:%u, device=%s\n",
                  mqttBroker.c_str(), mqttPort, deviceIdStr.c_str());
    Serial.printf("[MQTT] SOS topic: %s | Token topic: %s\n",
                  topicSos.c_str(), topicToken.c_str());
}

// ============================================================================
//  CONNEXION (avec LWT)
// ============================================================================
bool connectMqtt()
{
    if (mqtt.connected())
        return true;
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[MQTT] Wi-Fi down, skip MQTT connect");
        return false;
    }

    Serial.printf("[MQTT] Connecting to %s:%u...\n",
                  mqttBroker.c_str(), mqttPort);
    setLED(LED_MQTT_CONNECTING);

    bool connected = mqtt.connect(
        deviceIdStr.c_str(),
        nullptr, nullptr,          // pas d'auth pour les tests
        topicLwt.c_str(), 1, true, // LWT QoS 1 + retain
        "{\"state\":\"offline\"}");

    if (!connected)
    {
        Serial.printf("[MQTT] Failed, rc=%d\n", mqtt.state());
        return false;
    }

    Serial.println("[MQTT] Connected");
    setLED(LED_MQTT_OK);

    // Publier "online" (retain) et s'abonner aux notifs OTA + token
    mqtt.publish(topicLwt.c_str(), "{\"state\":\"online\"}", true);
    mqtt.subscribe(topicOtaNotify.c_str(), 1);
    mqtt.subscribe(topicToken.c_str(), 1);
    Serial.printf("[MQTT] Subscribed to %s and %s\n",
                  topicOtaNotify.c_str(), topicToken.c_str());

    return true;
}

// ============================================================================
//  POMPE DES MESSAGES ENTRANTS
// ============================================================================
void mqttLoopTick()
{
    mqtt.loop();
}

bool isMqttConnected()
{
    return mqtt.connected();
}

// ============================================================================
//  PUBLICATIONS
// ============================================================================

bool mqttPublishAlert(const String &jsonPayload)
{
    if (!mqtt.connected())
    {
        Serial.println("[MQTT] Cannot publish alert: not connected");
        return false;
    }

    Serial.printf("[MQTT] PUB alert (%u bytes) on %s\n",
                  jsonPayload.length(), topicAlert.c_str());

    setLED(LED_WIFI_SENDING);
    bool ok = mqtt.publish(topicAlert.c_str(),
                           (const uint8_t *)jsonPayload.c_str(),
                           jsonPayload.length(),
                           false); // pas retain

    if (ok)
    {
        Serial.println("[MQTT] Alert published OK");
        setLED(LED_SUCCESS);
    }
    else
    {
        Serial.printf("[MQTT] PUB failed, state=%d\n", mqtt.state());
        setLED(LED_ERROR);
    }
    return ok;
}

bool mqttPublishTelemetry(const String &jsonPayload)
{
    if (!mqtt.connected())
        return false;

    Serial.printf("[MQTT] PUB telemetry (%u bytes)\n", jsonPayload.length());
    return mqtt.publish(topicTelemetry.c_str(),
                        (const uint8_t *)jsonPayload.c_str(),
                        jsonPayload.length(),
                        false);
}

bool mqttPublishOtaStatus(const char *jsonPayload)
{
    if (!mqtt.connected())
        return false;
    return mqtt.publish(topicOtaStatus.c_str(), jsonPayload, false);
}

bool mqttPublishState(const String &jsonPayload)
{
    if (!mqtt.connected())
    {
        Serial.println("[MQTT] Cannot publish state: not connected");
        return false;
    }

    Serial.printf("[MQTT] PUB state (%u bytes) on %s\n",
                  jsonPayload.length(), topicState.c_str());

    bool ok = mqtt.publish(topicState.c_str(),
                           (const uint8_t *)jsonPayload.c_str(),
                           jsonPayload.length(),
                           true); // ← retain=true (point clé !)

    if (ok)
    {
        Serial.println("[MQTT] State published OK (retained)");
    }
    else
    {
        Serial.printf("[MQTT] State PUB failed, state=%d\n", mqtt.state());
    }
    return ok;
}

// ============================================================================
//  SOS — format plateforme HEELPMEE (helpme/{id}/sos)
// ============================================================================
bool mqttPublishSos(const String &recordId, double latitude, double longitude,
                    const char *isoTimestamp, float batteryLevel, int rssi)
{
    if (!mqtt.connected())
    {
        Serial.println("[MQTT] Cannot publish SOS: not connected");
        return false;
    }

    JsonDocument doc;
    doc["device_id"]     = deviceIdStr;
    doc["latitude"]      = latitude;
    doc["longitude"]     = longitude;
    doc["alert_type"]    = "sos";
    doc["timestamp"]     = isoTimestamp;
    doc["record_id"]     = recordId;
    doc["is_recording"]  = true;
    doc["battery_level"] = batteryLevel;
    doc["rssi"]          = rssi;

    char payload[512];
    size_t payloadLen = serializeJson(doc, payload, sizeof(payload));

    Serial.printf("[MQTT] PUB sos (%u bytes) on %s\n", payloadLen, topicSos.c_str());

    bool ok = mqtt.publish(topicSos.c_str(),
                           (const uint8_t *)payload, payloadLen,
                           false); // ephemeral, pas retain

    if (ok) {
        Serial.println("[MQTT] SOS published OK");
        Serial.printf("[MQTT] Payload: %s\n", payload);
    } else {
        Serial.printf("[MQTT] SOS PUB failed, state=%d\n", mqtt.state());
    }
    return ok;
}