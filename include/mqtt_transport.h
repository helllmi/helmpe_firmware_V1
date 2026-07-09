/**
 * ===========================================================================
 *  mqtt_transport.h — Couche neutre de transport MQTT
 * ===========================================================================
 *  Le code métier (main, FSM, storage flush, OTA) appelle ces fonctions
 *  sans savoir si le transport est WiFi ou LTE.
 *  Le routage se fait à l'exécution via bootMode_isForceWifi() + WiFi.status().
 * ===========================================================================
 */
#ifndef MQTT_TRANSPORT_H
#define MQTT_TRANSPORT_H

#include <Arduino.h>

// Init : se connecte au broker via le transport actif
bool mqttTransport_begin(const char* broker, uint16_t port, const char* clientId);

// État connexion
bool mqttTransport_isConnected();

// Reconnexion (à appeler depuis loop() si déconnecté)
bool mqttTransport_reconnect();

// Tick (à appeler régulièrement dans loop)
void mqttTransport_loop();

// Publication
bool mqttTransport_publishAlert(const String& payload);
bool mqttTransport_publishTelemetry(const String& payload);
bool mqttTransport_publishState(const String& payload);
bool mqttTransport_publishOtaStatus(const String& payload);

// Publie l'alerte SOS (helpme/{id}/sos) — route automatiquement LTE/WiFi
bool mqttTransport_publishSos(const String& recordId, double latitude, double longitude,
                              const char* isoTimestamp, float batteryLevel, int rssi);

// Abonnements (souscrit aux topics descendants)
bool mqttTransport_subscribeAll();

// Callback de réception (le handler reçoit topic + payload)
typedef void (*MqttRxCallback)(const String& topic, const String& payload);
void mqttTransport_onMessage(MqttRxCallback cb);

// Nom lisible du transport actif ("WIFI" ou "LTE") — pour les logs/JSON
const char* mqttTransport_typeName();

// Retourne true si le transport actif est WiFi (fallback).
// Utilisé par ota.cpp pour router vers performOTA_WiFi ou performOTA_LTE.
bool mqttTransport_isWifi();


#endif