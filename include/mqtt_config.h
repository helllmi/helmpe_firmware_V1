/**
 * mqtt_config.h — Configuration MQTT persistante (modifiable via portail)
 *
 * Charge au boot les paramètres depuis /mqtt.json sur LittleFS.
 * Si le fichier n'existe pas, utilise les valeurs par défaut de config.h.
 * Les paramètres peuvent être modifiés par le portail captif et sauvegardés.
 * Les changements prennent effet au prochain reboot.
 */
#ifndef MQTT_CONFIG_H
#define MQTT_CONFIG_H

#include <Arduino.h>

struct MqttConfig {
    String   broker;
    uint16_t port;
    String   clientId;
};

// Init : charge depuis /mqtt.json si présent, sinon utilise les défauts de config.h
bool mqttConfig_init();

// Récupère la config active (utilisée par mqttTransport_begin)
MqttConfig mqttConfig_get();

// Sauvegarde de nouveaux paramètres (effet au prochain reboot)
bool mqttConfig_set(const String& broker, uint16_t port, const String& clientId);

// Reset aux defaults (efface /mqtt.json)
bool mqttConfig_reset();

// True si une config custom est active (différente des defaults)
bool mqttConfig_isCustom();

#endif