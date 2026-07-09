/**
 * wifi_creds.h — Stockage persistant des credentials WiFi de fallback
 *
 * Sauvegardés sur LittleFS dans /wifi.json.
 * Survivent au reboot, modifiables via le portail captif.
 */
#ifndef WIFI_CREDS_H
#define WIFI_CREDS_H

#include <Arduino.h>

struct WifiCreds {
    String ssid;
    String password;
};

// Init : monte LittleFS si pas déjà fait, charge les creds en mémoire
bool wifiCreds_init();

// Lire les credentials actuels
WifiCreds wifiCreds_get();

// Sauvegarder de nouveaux credentials
bool wifiCreds_set(const String& ssid, const String& password);

// Effacer
bool wifiCreds_clear();

// Test true si on a un SSID enregistré
bool wifiCreds_hasSsid();

#endif