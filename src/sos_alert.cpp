#include <Arduino.h>
#include "sos_alert.h"
#include "config.h"
#include "mqtt_transport.h"
#include "gps.h"
#include "battery.h"
#include "lte.h"

// ============================================================================
//  ÉTAT INTERNE
// ============================================================================
static bool   alertInProgress = false;
static String currentRecordId = "";

// ============================================================================
//  UUID v4 — même approche que la plateforme de référence (MAC + random)
// ============================================================================
static void generateUUID(char *out, size_t len)
{
    uint64_t mac = ESP.getEfuseMac();
    uint32_t r1 = esp_random();
    uint32_t r2 = esp_random();
    uint32_t r3 = esp_random();
    uint32_t ms = millis();

    uint32_t timeLow = ms ^ r1;
    uint16_t timeMid = (uint16_t)((r2 >> 16) ^ (mac >> 32));
    uint16_t timeHiAndVersion = (uint16_t)((r2 & 0x0FFF) | 0x4000);
    uint8_t  clockSeqHiAndReserved = (uint8_t)((r3 & 0x3F) | 0x80);
    uint8_t  clockSeqLow = (uint8_t)((r3 >> 8) & 0xFF);
    uint64_t node = mac ^ ((uint64_t)r3 << 16);

    snprintf(out, len,
             "%08x-%04x-%04x-%02x%02x-%012llx",
             timeLow, timeMid, timeHiAndVersion,
             clockSeqHiAndReserved, clockSeqLow,
             (unsigned long long)node);
}

// ============================================================================
//  API PUBLIQUE
// ============================================================================
void sosAlert_trigger()
{
    if (alertInProgress) {
        Serial.println("[SOS] Alert already in progress, ignoring");
        return;
    }
    alertInProgress = true;

    // 1) Générer record_id
    char uuid[37];
    generateUUID(uuid, sizeof(uuid));
    currentRecordId = String(uuid);

    // 2) Position : GPS si fix valide, sinon défaut config.h
    double lat = SOS_DEFAULT_LATITUDE;
    double lon = SOS_DEFAULT_LONGITUDE;
    if (currentGPS.valid) {
        lat = currentGPS.latitude;
        lon = currentGPS.longitude;
    } else {
        Serial.println("[SOS] No GPS fix, using default location");
    }

    // 3) Timestamp réseau (fallback simple si vide)
    String timestamp = getNetworkTime();
    if (timestamp.length() == 0) {
        timestamp = "1970-01-01T00:00:00.000Z";
        Serial.println("[SOS] No network time, using epoch fallback");
    }

    // 4) Batterie + signal
    float battery = readBattery();
    SignalInfo sig = getSignalInfo();

    Serial.println("=== SOS FLOW STARTED ===");
    Serial.printf("[SOS] record_id=%s\n", currentRecordId.c_str());
    Serial.printf("[SOS] timestamp=%s\n", timestamp.c_str());
    Serial.printf("[SOS] lat=%.6f lon=%.6f battery=%.0f%% rssi=%d\n",
                  lat, lon, battery, sig.rssi);

    // 5) Publication MQTT (helpme/{id}/sos)
    bool ok = mqttTransport_publishSos(currentRecordId, lat, lon,
                                       timestamp.c_str(), battery, sig.rssi);
    if (!ok) {
        Serial.println("[SOS] WARNING: MQTT publish failed (will not block recording)");
    }

    Serial.println("=== SOS FLOW: MQTT done, audio recording handled separately ===");
    // L'enregistrement audio est déjà déclenché par state_machine.cpp
    // (audio_startRecording()). L'upload sera déclenché séparément
    // quand l'enregistrement se termine (voir patch state_machine.cpp).
}

const String& sosAlert_getCurrentRecordId()
{
    return currentRecordId;
}

void sosAlert_reset()
{
    alertInProgress = false;
    currentRecordId = "";
}