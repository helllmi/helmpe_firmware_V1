#include <Arduino.h>
#include <WiFi.h>
#include "fallbackwifi.h"
#include "secrets.h"

// ============================================================================
//  CONFIGURATION WI-FI
// ============================================================================
// Les credentials sont définis ici. Plus tard tu peux les externaliser
// en NVS ou via un fichier de config sur la SD.


// ============================================================================
//  CONNEXION
// ============================================================================
// Bloque jusqu'à connexion ou timeout. Ne reboote JAMAIS en cas d'échec.
bool connectWiFi(uint32_t timeoutMs) {
    if (WiFi.status() == WL_CONNECTED) return true;

    Serial.printf("[WIFI] Connecting to %s...\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
        delay(500);
        Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WIFI] Connected. IP: %s\n",
                      WiFi.localIP().toString().c_str());
        return true;
    }

    Serial.println("\n[WIFI] Connection failed");
    return false;
}

bool isWiFiConnected() {
    return WiFi.status() == WL_CONNECTED;
}
