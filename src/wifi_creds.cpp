#include <Arduino.h>
#include "wifi_creds.h"
#include <LittleFS.h>

#define WIFI_CREDS_FILE  "/wifi.json"

static WifiCreds cachedCreds;
static bool loaded = false;

// ============================================================================
//  HELPERS
// ============================================================================
static bool ensureLittleFS() {
    // LittleFS est déjà monté par storage_init(). On vérifie juste.
    return LittleFS.begin(false);
}

static void loadFromFile() {
    cachedCreds.ssid = "";
    cachedCreds.password = "";

    if (!LittleFS.exists(WIFI_CREDS_FILE)) {
        loaded = true;
        return;
    }

    File f = LittleFS.open(WIFI_CREDS_FILE, "r");
    if (!f) {
        loaded = true;
        return;
    }

    // Format simple : 1ère ligne = SSID, 2ème = password
    cachedCreds.ssid = f.readStringUntil('\n');
    cachedCreds.ssid.trim();
    cachedCreds.password = f.readStringUntil('\n');
    cachedCreds.password.trim();
    f.close();

    loaded = true;
    Serial.printf("[WCREDS] Loaded SSID='%s' (password length=%u)\n",
                  cachedCreds.ssid.c_str(), cachedCreds.password.length());
}

// ============================================================================
//  API
// ============================================================================
bool wifiCreds_init() {
    if (!ensureLittleFS()) {
        Serial.println("[WCREDS] LittleFS not available");
        return false;
    }
    loadFromFile();
    return true;
}

WifiCreds wifiCreds_get() {
    if (!loaded) loadFromFile();
    return cachedCreds;
}

bool wifiCreds_set(const String& ssid, const String& password) {
    File f = LittleFS.open(WIFI_CREDS_FILE, "w");
    if (!f) {
        Serial.println("[WCREDS] Cannot open file for write");
        return false;
    }
    f.println(ssid);
    f.println(password);
    f.close();

    cachedCreds.ssid = ssid;
    cachedCreds.password = password;
    Serial.printf("[WCREDS] Saved SSID='%s'\n", ssid.c_str());
    return true;
}

bool wifiCreds_clear() {
    cachedCreds.ssid = "";
    cachedCreds.password = "";
    if (LittleFS.exists(WIFI_CREDS_FILE)) {
        LittleFS.remove(WIFI_CREDS_FILE);
    }
    Serial.println("[WCREDS] Cleared");
    return true;
}

bool wifiCreds_hasSsid() {
    if (!loaded) loadFromFile();
    return cachedCreds.ssid.length() > 0;
}