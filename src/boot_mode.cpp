#include <Arduino.h>
#include "boot_mode.h"
#include <Preferences.h>

static Preferences prefs;
static const char* NVS_NAMESPACE = "bootmode";
static const char* KEY_FORCE_WIFI = "force_wifi";

void bootMode_init() {
    // Ouvre le namespace NVS
    prefs.begin(NVS_NAMESPACE, false);
    bool force = prefs.getBool(KEY_FORCE_WIFI, false);
    Serial.printf("[BOOT] force_wifi flag: %s\n", force ? "TRUE" : "false");
    prefs.end();
}

bool bootMode_isForceWifi() {
    prefs.begin(NVS_NAMESPACE, true);   // read-only
    bool f = prefs.getBool(KEY_FORCE_WIFI, false);
    prefs.end();
    return f;
}

void bootMode_setForceWifi(bool force) {
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putBool(KEY_FORCE_WIFI, force);
    prefs.end();
    Serial.printf("[BOOT] force_wifi set to %s\n", force ? "TRUE" : "false");
}

void bootMode_reset() {
    bootMode_setForceWifi(false);
}