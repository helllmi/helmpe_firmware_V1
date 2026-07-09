#include <Arduino.h>
#include "mqtt_config.h"
#include "config.h"
#include <LittleFS.h>

#define MQTT_CONFIG_FILE  "/mqtt.json"

static MqttConfig cachedConfig;
static bool       loaded = false;
static bool       isCustom = false;

// ============================================================================
//  HELPERS
// ============================================================================

// Charge les defaults depuis config.h
static void loadDefaults() {
    cachedConfig.broker   = MQTT_BROKER_HOST;
    cachedConfig.port     = MQTT_BROKER_PORT;
    cachedConfig.clientId = DEVICE_ID;
    isCustom = false;
}

// Charge depuis fichier si présent
static void loadFromFile() {
    loadDefaults();  // d'abord les defaults

    if (!LittleFS.begin(false)) {
        Serial.println("[MCONF] LittleFS not available, using defaults");
        loaded = true;
        return;
    }

    if (!LittleFS.exists(MQTT_CONFIG_FILE)) {
        Serial.println("[MCONF] No custom config, using defaults");
        loaded = true;
        return;
    }

    File f = LittleFS.open(MQTT_CONFIG_FILE, "r");
    if (!f) {
        loaded = true;
        return;
    }

    // Format simple : 1 paramètre par ligne (broker, port, clientId)
    String broker   = f.readStringUntil('\n'); broker.trim();
    String portStr  = f.readStringUntil('\n'); portStr.trim();
    String clientId = f.readStringUntil('\n'); clientId.trim();
    f.close();

    if (broker.length() > 0 && portStr.length() > 0) {
        cachedConfig.broker   = broker;
        cachedConfig.port     = (uint16_t)portStr.toInt();
        cachedConfig.clientId = clientId.length() > 0 ? clientId : String(DEVICE_ID);
        isCustom = true;
        Serial.printf("[MCONF] Loaded custom: broker=%s port=%u clientId=%s\n",
                      cachedConfig.broker.c_str(),
                      cachedConfig.port,
                      cachedConfig.clientId.c_str());
    }

    loaded = true;
}

// ============================================================================
//  API
// ============================================================================
bool mqttConfig_init() {
    loadFromFile();
    return true;
}

MqttConfig mqttConfig_get() {
    if (!loaded) loadFromFile();
    return cachedConfig;
}

bool mqttConfig_set(const String& broker, uint16_t port, const String& clientId) {
    if (!LittleFS.begin(false)) return false;

    File f = LittleFS.open(MQTT_CONFIG_FILE, "w");
    if (!f) {
        Serial.println("[MCONF] Cannot open file for write");
        return false;
    }
    f.println(broker);
    f.println(port);
    f.println(clientId);
    f.close();

    cachedConfig.broker   = broker;
    cachedConfig.port     = port;
    cachedConfig.clientId = clientId;
    isCustom = true;

    Serial.printf("[MCONF] Saved: broker=%s port=%u clientId=%s\n",
                  broker.c_str(), port, clientId.c_str());
    return true;
}

bool mqttConfig_reset() {
    if (LittleFS.exists(MQTT_CONFIG_FILE)) {
        LittleFS.remove(MQTT_CONFIG_FILE);
    }
    loadDefaults();
    Serial.println("[MCONF] Reset to defaults");
    return true;
}

bool mqttConfig_isCustom() {
    if (!loaded) loadFromFile();
    return isCustom;
}