
#ifndef MQTT_LTE_H
#define MQTT_LTE_H

#include <Arduino.h>

//calbback pour la réception d'un message MQTT (topic + payload)
typedef void (*MqttLteCallback)(const String& topic, const String& payload);
 void mqttLte_onMessage(MqttLteCallback cb);


bool mqttLte_start(const char* broker, uint16_t port, const char* clientId);
// Publie un message sur un topic.
// Séquence : CMQTTTOPIC → CMQTTPAYLOAD → CMQTTPUB.
// qos : 0 ou 1. retain : true/false.
bool mqttLte_publish(const char* topic, const char* payload, uint8_t qos, bool retain);

// S'abonne à un topic (pour recevoir les messages descendants : OTA, config...).
bool mqttLte_subscribe(const char* topic, uint8_t qos);

// À appeler régulièrement : surveille les messages entrants (URC).
void mqttLte_loop();
void mqttLte_feedUrc(const String& urcLine);
// Message MQTT complet reçu (après +CMQTTRXSTART...+CMQTTRXEND)
void mqttLte_feedMessage(const String& topic, const String& payload);
// Déconnecte proprement et arrête le service MQTT du modem.
void mqttLte_stop();
bool mqttLte_isConnected();
bool mqttLte_needsRestart();


#endif