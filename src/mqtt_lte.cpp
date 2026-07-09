#include <Arduino.h>
#include "mqtt_lte.h"
#include "serial_comm.h"
#include "config.h"

// ============================================================================
//  ÉTAT INTERNE
// ============================================================================
static bool connected = false;
static bool needsRestart = false;
static MqttLteCallback messageCallback = nullptr;

#define MQTT_CLIENT_INDEX 0

// ============================================================================
//  NETTOYAGE MQTT (à appeler avant un (re)démarrage)
// ============================================================================
static void mqttLte_cleanup()
{
    Serial.println("[MQTT-LTE] Cleanup...");

    char cmd[32];

    snprintf(cmd, sizeof(cmd), "AT+CMQTTDISC=%d,120", MQTT_CLIENT_INDEX);
    SentMessage(cmd, 5000);
    delay(1000);

    snprintf(cmd, sizeof(cmd), "AT+CMQTTREL=%d", MQTT_CLIENT_INDEX);
    SentMessage(cmd, 5000);
    delay(1000);

    SentMessageAsync("AT+CMQTTSTOP", "+CMQTTSTOP:", 8000);
    delay(1500);
}

// ============================================================================
//  DÉMARRAGE + CONNEXION
// ============================================================================
bool mqttLte_start(const char *broker, uint16_t port, const char *clientId)
{
    Serial.println("[MQTT-LTE] Starting MQTT service over LTE...");
    connected    = false;
    needsRestart = false;

    mqttLte_cleanup();

    String startResp = SentMessageAsync("AT+CMQTTSTART", "+CMQTTSTART:", 15000);
    Serial.printf("[MQTT-LTE] CMQTTSTART resp: %s\n", startResp.c_str());

    bool startOk = startResp.indexOf("+CMQTTSTART: 0") != -1 ||
                   startResp.indexOf("+CMQTTSTART: 1") != -1 ||
                   startResp.indexOf("+CMQTTSTART: 3") != -1;

    if (!startOk) {
        Serial.println("[MQTT-LTE] CMQTTSTART failed");
        return false;
    }
    Serial.println("[MQTT-LTE] CMQTTSTART OK");
    delay(500);

    char accqCmd[128];
    snprintf(accqCmd, sizeof(accqCmd),
             "AT+CMQTTACCQ=%d,\"%s\",0",
             MQTT_CLIENT_INDEX, clientId);

    if (!SentMessage(accqCmd, 8000)) {
        Serial.println("[MQTT-LTE] CMQTTACCQ failed");
        return false;
    }
    Serial.println("[MQTT-LTE] CMQTTACCQ OK");
    delay(500);

    char connectCmd[192];
    snprintf(connectCmd, sizeof(connectCmd),
             "AT+CMQTTCONNECT=%d,\"tcp://%s:%u\",%d,1",
             MQTT_CLIENT_INDEX, broker, port, MQTT_KEEPALIVE_SEC);

    String connResp = SentMessageAsync(connectCmd, "+CMQTTCONNECT:", 25000);
    Serial.printf("[MQTT-LTE] CMQTTCONNECT resp: %s\n", connResp.c_str());

    if (connResp.indexOf("+CMQTTCONNECT: 0,0") != -1) {
        connected = true;
        Serial.println("[MQTT-LTE] Connected to broker");
        return true;
    }

    Serial.println("[MQTT-LTE] Connect failed");
    return false;
}

// ============================================================================
//  GETTERS / STOP
// ============================================================================
bool mqttLte_isConnected()
{
    return connected;
}

bool mqttLte_needsRestart()
{
    return needsRestart;
}

void mqttLte_stop()
{
    mqttLte_cleanup();
    connected    = false;
    needsRestart = false;
}

// ============================================================================
//  ABONNEMENT
// ============================================================================
bool mqttLte_subscribe(const char *topic, uint8_t qos)
{
    if (!connected) {
        Serial.println("[MQTT-LTE] Cannot subscribe: not connected");
        return false;
    }

    size_t topicLen = strlen(topic);

    char subCmd[48];
    snprintf(subCmd, sizeof(subCmd),
             "AT+CMQTTSUB=%d,%u,%u",
             MQTT_CLIENT_INDEX, (unsigned)topicLen, qos);

    if (!SentPrompt(subCmd, topic, 10000)) {
        Serial.println("[MQTT-LTE] CMQTTSUB failed");
        return false;
    }

    Serial.printf("[MQTT-LTE] Subscribed to %s\n", topic);
    return true;
}

// ============================================================================
//  PUBLICATION (protégée par modem mutex)
// ============================================================================
bool mqttLte_publish(const char *topic, const char *payload,
                     uint8_t qos, bool retain)
{
    if (!connected) {
        Serial.println("[MQTT-LTE] Cannot publish: not connected");
        return false;
    }

    // Ne pas publier si le modem est verrouillé (upload LTE en cours)
    if (!modemMutex_tryTake()) {
        Serial.println("[MQTT-LTE] Modem busy (upload in progress), skipping publish");
        return false;
    }

    size_t topicLen   = strlen(topic);
    size_t payloadLen = strlen(payload);

    // 1) Topic
    char topicCmd[48];
    snprintf(topicCmd, sizeof(topicCmd),
             "AT+CMQTTTOPIC=%d,%u",
             MQTT_CLIENT_INDEX, (unsigned)topicLen);
    if (!SentPrompt(topicCmd, topic, 8000)) {
        Serial.println("[MQTT-LTE] CMQTTTOPIC failed");
        connected = false;
        modemMutex_give();
        return false;
    }

    // 2) Payload
    char payloadCmd[48];
    snprintf(payloadCmd, sizeof(payloadCmd),
             "AT+CMQTTPAYLOAD=%d,%u",
             MQTT_CLIENT_INDEX, (unsigned)payloadLen);
    if (!SentPrompt(payloadCmd, payload, 8000)) {
        Serial.println("[MQTT-LTE] CMQTTPAYLOAD failed");
        connected = false;
        modemMutex_give();
        return false;
    }

    // 3) Publier
    char pubCmd[48];
    snprintf(pubCmd, sizeof(pubCmd),
             "AT+CMQTTPUB=%d,%u,60,%d",
             MQTT_CLIENT_INDEX, qos, retain ? 1 : 0);

    String r = SentMessageAsync(pubCmd, "+CMQTTPUB:", 15000);
    if (r.indexOf("+CMQTTPUB: 0,0") != -1) {
        Serial.printf("[MQTT-LTE] Published %u bytes on %s\n",
                      (unsigned)payloadLen, topic);
        modemMutex_give();
        return true;
    }

    Serial.printf("[MQTT-LTE] Publish failed: %s\n", r.c_str());
    connected = false;
    modemMutex_give();
    return false;
}

// ============================================================================
//  CALLBACK
// ============================================================================
void mqttLte_onMessage(MqttLteCallback cb)
{
    messageCallback = cb;
}

// ============================================================================
//  LOOP — plus de lecture UART ici (cf. modem_urc.cpp)
// ============================================================================
void mqttLte_loop()
{
    // No-op : les URC sont dispatchés par modem_urc.cpp
}

// ============================================================================
//  FEED URC — appelé par modem_urc.cpp pour les URC simples
// ============================================================================
void mqttLte_feedUrc(const String &line)
{
    if (line.startsWith("+CMQTTCONNLOST"))
    {
        Serial.printf("[MQTT-LTE] Connection lost: %s\n", line.c_str());
        connected = false;
    }
    else if (line.startsWith("+CMQTTNONET"))
    {
        Serial.println("[MQTT-LTE] No network! Restart required");
        connected    = false;
        needsRestart = true;
    }
}

// ============================================================================
//  FEED MESSAGE — appelé par modem_urc.cpp après capture complète
//                 d'une séquence +CMQTTRXSTART ... +CMQTTRXEND
// ============================================================================
void mqttLte_feedMessage(const String &topic, const String &payload)
{
    Serial.printf("[MQTT-LTE] RX on %s (%u bytes)\n",
                  topic.c_str(), payload.length());

    if (messageCallback != nullptr)
    {
        messageCallback(topic, payload);
    }
}