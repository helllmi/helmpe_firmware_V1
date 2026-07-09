/**
 * ===========================================================================
 *  Firmware ESP32-S3 — Alerte IoT  v1.0.0
 * ===========================================================================
 *  Fusion :
 *    - ESP-S3-ALERT  (GPS, batterie, NeoPixel, SIM7670G LTE, fallback Wi-Fi)
 *    - firmware-v1-mqtt (MQTT, OTA, watchdog, rollback)
 *
 *  Logique :
 *    - Toutes les 2s : lecture GPS via SIM7670G
 *    - Si fix valide : envoi d'une ALERTE par MQTT (toutes les 10s mini)
 *    - Toutes les 60s : envoi d'une TÉLÉMÉTRIE par MQTT
 *    - À tout moment : un OTA push peut arriver sur devices/{id}/ota/notify
 *      → téléchargement HTTP + SHA256 + Update + reboot + observation 1 min
 *
 *  Transport actuel : Wi-Fi (PubSubClient). Migration LTE Cat M1 ultérieure
 *  via AT+CMQTT* du SIM7670G — la couche métier reste identique.
 * ===========================================================================
 */
//--------------------------
#include <Preferences.h>
//-------------------
#include <Arduino.h>
#include <Wire.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include "config.h"
#include "sos_button.h"
#include "state_machine.h"
#include "serial_comm.h"
#include "battery.h"
#include "gps.h"
#include "lte.h"
#include "fallbackwifi.h"
#include "led.h"
#include "mqtt_client.h"
#include "ota.h"
#include "watchdog.h"
#include "storage.h"
#include "power_manager.h"
#include "mqtt_transport.h"
#include "audio.h"
#include "captive_portal.h"
#include "wifi_creds.h"
#include "mqtt_config.h"
#include "boot_mode.h"
#include "sms_handler.h"
#include "modem_urc.h"
//-------------------------------------
#include "jwt_storage.h"
#include "sos_alert.h"
#include "audio_upload.h"
//---------------------------------------
#include "voice_trigger.h"
#include "i2s_resource.h"

// ===========================================================================
//  ÉTAT GLOBAL
// ===========================================================================
unsigned long lastAlertMs = 0;
unsigned long lastTelemetryMs = 0;
unsigned long lastDeleteMs = 0;
unsigned long lastStateMs = 0;
bool alertPublishedForThisAction = false;
// ===========================================================================
//  CONSTRUCTION DES PAYLOADS JSON
// ===========================================================================

// Reprend ton format complet (compatible backend / CRM existant)
static String buildAlertJson()
{
    float batteryLevel = readBattery();
    String networkTime = getNetworkTime();
    SignalInfo sig = getSignalInfo();

    JsonDocument doc;

    doc["timestamp"] = networkTime.length() > 0
                           ? networkTime
                           : String(millis() / 1000);
    doc["priority"] = "CRITICAL";

    // LOCATION
    JsonObject loc = doc["location"].to<JsonObject>();
    loc["latitude"] = currentGPS.latitude;
    loc["longitude"] = currentGPS.longitude;
    loc["accuracy_meters"] = 5.0;
    loc["altitude_meters"] = currentGPS.altitude;
    loc["speed_kmh"] = currentGPS.speed;
    loc["heading_degrees"] = nullptr;
    loc["location_source"] = "GPS";
    loc["address_reverse_geocoded"] = "Tunisia";

    // CONTEXT
    JsonObject ctx = doc["context"].to<JsonObject>();
    ctx["fall_detected"] = false;
    ctx["heart_rate_bpm"] = 100;
    ctx["ambient_noise_db"] = 70.0;
    ctx["motion_state"] = "STATIONARY";
    ctx["geofence_status"] = "UNKNOWN";

    // METADATA
    JsonObject meta = doc["metadata"].to<JsonObject>();
    meta["schema_version"] = "1.0";
    meta["kafka_topic"] = "helpmee_alerts";
    meta["partition_key"] = DEVICE_ID;
    meta["ttl_seconds"] = 86400;
    meta["retry_count"] = 0;

    // IDs
    doc["alert_id"] = "ALT-" + String(DEVICE_ID);
    doc["device_id"] = DEVICE_ID;
    doc["user_id"] = "USR-001";
    doc["alert_type"] = "PANIC";
    doc["trigger_method"] = "BUTTON_PRESS";
    doc["press_count"] = sosButton_getPressCount();
    doc["press_duration_ms"] = sosButton_getPressDurationMs();

    // DEVICE STATUS
    JsonObject dev = doc["device_status"].to<JsonObject>();
    dev["charging"] = false;
    dev["battery_level_pct"] = (int)batteryLevel;
    dev["signal_strength_dbm"] = sig.rssi;
    dev["connectivity_type"] = isWiFiConnected() ? "WIFI" : "LTE";
    dev["firmware_version"] = FIRMWARE_VERSION;
    dev["is_charging"] = false;
    dev["last_health_check"] = millis() / 1000;

    // USER PROFILE
    JsonObject usr = doc["user_profile"].to<JsonObject>();
    usr["age"] = 78;
    usr["category"] = "ELDERLY";
    usr["full_name"] = "Fatma Ben Ali";
    usr["blood_type"] = "A+";

    JsonArray conds = usr["medical_conditions"].to<JsonArray>();
    conds.add("hypertension");
    conds.add("diabetes_type_2");

    JsonArray contacts = usr["emergency_contacts"].to<JsonArray>();
    JsonObject c = contacts.add<JsonObject>();
    c["name"] = "Ahmed Ben Ali";
    c["relationship"] = "SON";
    c["phone"] = "+216-55-123-456";
    c["notify"] = true;

    JsonArray langs = usr["spoken_languages"].to<JsonArray>();
    langs.add("ar");
    langs.add("fr");

    String out;
    serializeJson(doc, out);
    return out;
}

// Télémétrie courte, envoyée régulièrement
static String buildTelemetryJson()
{
    SignalInfo sig = getSignalInfo();

    JsonDocument doc;
    doc["device_id"] = DEVICE_ID;
    doc["timestamp"] = getNetworkTime();
    doc["battery_pct"] = (int)readBattery();
    doc["signal_rssi"] = sig.rssi;
    doc["signal_ber"] = sig.ber;
    doc["connectivity"] = isWiFiConnected() ? "WIFI" : "LTE";
    doc["firmware_version"] = FIRMWARE_VERSION;
    doc["uptime_seconds"] = millis() / 1000;
    doc["gps_valid"] = currentGPS.valid;

    String out;
    serializeJson(doc, out);
    return out;
}
static String buildStateJson()
{
    JsonDocument doc;

    doc["device_id"] = DEVICE_ID;
    doc["state"] = stateMachine_stateName(stateMachine_getState());
    doc["timestamp"] = getNetworkTime();
    doc["battery_pct"] = (int)readBattery();
    doc["uptime_seconds"] = millis() / 1000;
    doc["state_uptime_sec"] = stateMachine_getStateUptime() / 1000;
    doc["firmware_version"] = FIRMWARE_VERSION;
    doc["connectivity"] = mqttTransport_typeName();
    doc["gps_valid"] = currentGPS.valid;

    String out;
    serializeJson(doc, out);
    return out;
}
static void onStateChanged(DeviceState newState)
{
    // Réarmer pour la PROCHAINE alerte (nouvelle entrée en ACTION)
    if (newState == STATE_STANDBY)
    {
        alertPublishedForThisAction = false;
    }

    if (mqttTransport_isConnected())
    {
        String stateJson = buildStateJson();
        mqttTransport_publishState(stateJson);
    }
}
// callback pour republier une alerte stockée après reconnexion MQTT
static bool republishAlert(const String &payload)
{
    if (!mqttTransport_isConnected())
    {
        return false; // pas connecté → échec, le message reste en queue
    }
    return mqttTransport_publishAlert(payload);
}

SPIClass sdSpi(HSPI);

// ===========================================================================
//  SETUP
// ===========================================================================
void setup()
{
    // Alimentation périphériques externes
#if PIN_POWER_PERIPH >= 0
    pinMode(PIN_POWER_PERIPH, OUTPUT);
    digitalWrite(PIN_POWER_PERIPH, HIGH);
#endif

    Serial.begin(115200);
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);

    delay(3000);

    Serial.println("\n\n=========================================");
    Serial.printf("  ESP32-S3 ALERT FIRMWARE v%s\n", FIRMWARE_VERSION);
    Serial.printf("  Build:  %s %s\n", __DATE__, __TIME__);
    Serial.printf("  Device: %s\n", DEVICE_ID);
    Serial.println("=========================================\n");

    // LED
    ledBegin();
    setLED(LED_STARTUP);

    // Bouton SOS
    sosButton_init();

    // Watchdog hardware
    setupWatchdog();

    // Power manager
    power_init();

    // Stockage LittleFS
    storage_init();
    wifiCreds_init();
    bootMode_init();

    // Token JWT
    jwtStorage_load();

    // Config MQTT
    mqttConfig_init();

    // Diagnostic partitions OTA
    debugAllPartitions();

    // Observation post-OTA
    runSelfTestAfterOTA();

    // ── ÉTAPE 1 : LTE TOUJOURS EN PREMIER ──────────────────────────────────
    // setupLTE() efface le flag force_wifi si le modem répond et NETOPEN OK
    // Donc après setupLTE(), bootMode_isForceWifi() sera false si LTE marche
    setupLTE();

    SentMessage("AT+CTZU=1", 3000); // sync auto temps réseau
    delay(1000);

    Serial.printf("Battery: %.1f%%\n", readBattery());

    // GNSS
    Serial.println("Powering GNSS...");
    SentMessage("AT+CGNSSPWR=1", 5000);
    delay(3000);

    // Carte SD

    sdSpi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    if (!SD.begin(PIN_SD_CS, sdSpi))
    {
        Serial.println("SD Failed (non-critical)");
    }
    else
    {
        Serial.println("SD OK");
    }

    // I2S + Audio
    i2sResource_init();
    audio_init();

    // SMS + URC dispatcher + mutex modem
    smsHandler_init();
    modemUrc_init();
    modemMutex_init();
    /*//----------------------------------------
     File dbgDir = SD.open("/alerts");
    if (!dbgDir) {
        Serial.println("[SD] Cannot open /alerts");
    } else {
        Serial.printf("[SD] Dir isDirectory: %s\n", dbgDir.isDirectory() ? "YES" : "NO");
        int count = 0;
        File f = dbgDir.openNextFile();
        while (f && count < 20) {
            Serial.printf("[SD] Entry: name='%s' size=%u isDir=%s\n",
                          f.name(), f.size(), f.isDirectory() ? "YES" : "NO");
            f.close();
            f = dbgDir.openNextFile();
            count++;
        }
        if (count == 0) Serial.println("[SD] No entries found");
        dbgDir.close();
    }**/

    // ── ÉTAPE 2 : DÉCISION WiFi/LTE (APRÈS setupLTE()) ─────────────────────
    // Si setupLTE() a effacé le flag → forceWifi = false → on reste en LTE
    // Si le LTE a déjà échoué 3x avant ce boot → forceWifi = true → WiFi
    bool forceWifi = bootMode_isForceWifi();
    if (forceWifi)
    {
        Serial.println("[MAIN] *** FORCE WIFI MODE *** (LTE fallback after failures)");

        if (!wifiCreds_hasSsid())
        {
            Serial.println("[MAIN] No WiFi creds — reverting to LTE");
            bootMode_setForceWifi(false);
            forceWifi = false;
        }
        else
        {
            WifiCreds creds = wifiCreds_get();
            Serial.printf("[MAIN] Connecting WiFi to '%s'...\n", creds.ssid.c_str());

            WiFi.mode(WIFI_STA);
            WiFi.begin(creds.ssid.c_str(), creds.password.c_str());

            uint32_t start = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - start < 15000)
            {
                delay(500);
                Serial.print(".");
            }
            Serial.println();

            if (WiFi.status() == WL_CONNECTED)
            {
                Serial.printf("[MAIN] WiFi connected! IP: %s\n",
                              WiFi.localIP().toString().c_str());
            }
            else
            {
                Serial.println("[MAIN] WiFi connection FAILED — reverting to LTE");
                bootMode_setForceWifi(false);
                forceWifi = false;
                WiFi.disconnect(true);
                WiFi.mode(WIFI_OFF);
            }
        }
    }

    // ── ÉTAPE 3 : MQTT (WiFi ou LTE selon décision ci-dessus) ──────────────
    MqttConfig mcfg = mqttConfig_get();
    mqttTransport_begin(mcfg.broker.c_str(), mcfg.port, mcfg.clientId.c_str());
    Serial.printf("[MAIN] MQTT config: broker=%s port=%u clientId=%s%s\n",
                  mcfg.broker.c_str(), mcfg.port, mcfg.clientId.c_str(),
                  mqttConfig_isCustom() ? " (custom)" : " (default)");

    if (isWiFiConnected())
    {
        mqttTransport_reconnect();
    }

    lastDeleteMs = millis();
    stateMachine_init();
    stateMachine_onStateChange(onStateChanged);
    stateMachine_dispatch(EVT_BOOT_OK);

    Serial.println("\n[INFO] Setup complete - entering loop()\n");
}

// ============================================================================
//  HELPER : tick du bouton + routage des événements selon l'état FSM
// ============================================================================
// Comportements :
//   En STANDBY :
//     - LONG PRESS (≥ 3s)     → déclenche le SOS (passage en ACTION)
//     - TRIPLE CLIC           → ouvre le portail captif
//   En ACTION :
//     - LONG PRESS OU TRIPLE  → reset utilisateur (retour en STANDBY)
static void handleButtonEvents()
{
    sosButton_tick();
    DeviceState s = stateMachine_getState();

    // ── Long press : SOS si STANDBY, reset si ACTION ───────────────────────
    if (sosButton_wasLongPressed())
    {
        Serial.printf("[MAIN] LONG PRESS event (duration=%dms)\n",
                      sosButton_getPressDurationMs());

        if (s == STATE_STANDBY)
        {
            Serial.println("[MAIN] → Triggering SOS via long press");
            stateMachine_dispatch(EVT_SOS_TRIGGERED);
        }
        else if (s == STATE_ACTION)
        {
            Serial.println("[MAIN] → User reset via long press");
            stateMachine_dispatch(EVT_USER_RESET);
        }
        return; // long press traité, on ne traite pas le triple dans la même tick
    }

    // ── Triple clic : portail captif si STANDBY, reset si ACTION ───────────
    if (sosButton_wasTripleClicked())
    {
        Serial.printf("[MAIN] TRIPLE CLICK event (count=%d, duration=%dms)\n",
                      sosButton_getPressCount(),
                      sosButton_getPressDurationMs());

        if (s == STATE_STANDBY)
        {
            Serial.println("[MAIN] → Opening captive portal");
            captivePortal_start();
        }
        else if (s == STATE_ACTION)
        {
            Serial.println("[MAIN] → User reset via triple click");
            stateMachine_dispatch(EVT_USER_RESET);
        }
    }
}

// ===========================================================================
//  LOOP
// ===========================================================================
void loop()
{
    feedWatchdog();

    // ── DIAGNOSTIC TEMPORAIRE — à retirer ensuite ──
    static uint32_t lastLoopTime = 0;
    uint32_t now = millis();
    uint32_t cycleTime = now - lastLoopTime;
    lastLoopTime = now;
    if (cycleTime > 200)
    {
        Serial.printf("[LOOP] cycle = %u ms\n", cycleTime);
    }

    // ── Boutons SOS (long press + triple clic) ──────────────────────────────
    handleButtonEvents();
    stateMachine_tick();

    // ── 2. Maintenance MQTT ─────────────────────────────────────────────────
    static uint32_t lastMqttRetry = 0;
    if (!mqttTransport_isConnected() &&
        millis() - lastMqttRetry > MQTT_RETRY_INTERVAL_MS)
    {
        lastMqttRetry = millis();

        // Si on est en WiFi fallback, vérifier si le LTE est revenu
        if (bootMode_isForceWifi())
        {
            Serial.println("[MAIN] WiFi mode — checking if LTE is back...");
            String netCheck = SentMessageResponse("AT+NETOPEN?", 3000);
            if (netCheck.indexOf("+NETOPEN: 1") != -1)
            {
                // LTE réseau disponible → revenir au LTE
                Serial.println("[MAIN] LTE back — switching from WiFi to LTE");
                bootMode_setForceWifi(false);
                WiFi.disconnect(true);
                WiFi.mode(WIFI_OFF);
                delay(500);
                ESP.restart(); // reboot propre pour réinitialiser le transport
            }
        }

        mqttTransport_reconnect();
    }
    mqttTransport_loop();

    static bool wasMqttConnected = false;
    bool nowConnected = mqttTransport_isConnected();
    if (nowConnected && !wasMqttConnected)
    {
        size_t pending = storage_count();
        if (pending > 0)
        {
            Serial.printf("[MAIN] MQTT reconnected — %u buffered alerts to flush\n",
                          (unsigned)pending);
        }
    }
    wasMqttConnected = nowConnected;

    // ── 3. Observation post-OTA ─────────────────────────────────────────────
    if (isOtaObserving())
    {
        ledHeartbeatTick(LED_OTA_OBSERVE, 250);
    }

    // ── 4. Rotation du log GPS sur SD ──────────────────────────────────────
    if (millis() - lastDeleteMs >= DELETE_LOG_INTERVAL_MS)
    {
        if (SD.exists("/gps_log.csv"))
        {
            Serial.println("Deleting SD log...");
            SD.remove("/gps_log.csv");
        }
        lastDeleteMs = millis();
    }

    // ── 5. Lecture GPS ──────────────────────────────────────────────────────
    /** bool gpsOk = readGPS();
     if (gpsOk)
     {
         Serial.printf("GPS: %.6f, %.6f\n",
                       currentGPS.latitude, currentGPS.longitude);
         if (!isOtaObserving())
             setLED(LED_GPS_OK);
         logGPS();
     }
     else
     {
         Serial.println("GPS: waiting for fix...");
         if (!isOtaObserving())
             setLED(LED_GPS_SEARCH);
     }**/

    // ── 6. Envoi ALERTE MQTT ────────────────────────────────────────────────
    if (stateMachine_getState() == STATE_ACTION && !alertPublishedForThisAction)
    {
        alertPublishedForThisAction = true; // ne pas republier tant qu'on reste en ACTION

        // Construire le JSON
        String payload = buildAlertJson();
        Serial.println("================ ALERT JSON ================");
        Serial.println(payload);
        Serial.printf("Size: %u bytes\n", payload.length());
        Serial.println("============================================");

        // Tenter la publication
        bool published = false;
        if (mqttTransport_isConnected())
        {
            published = mqttTransport_publishAlert(payload);
        }

        // Si pas connecté OU si la publication a échoué → bufferiser
        if (!published)
        {
            Serial.println("[ALERT] Publish failed — buffering to flash");
            setLED(LED_ERROR);
            storage_enqueue(payload);
        }
    }

    // ── Rejeu du buffer offline ─────────────────────────────────────────────
    if (mqttTransport_isConnected() && storage_count() > 0)
    {
        storage_flush(republishAlert);
    }

    // ── 7. Envoi TÉLÉMÉTRIE MQTT  ─────────────────────
    if (stateMachine_getState() == STATE_STANDBY &&
        millis() - lastTelemetryMs > TELEMETRY_INTERVAL_STANDBY_MS)
    {
        lastTelemetryMs = millis();
        if (mqttTransport_isConnected())
        {
            String tele = buildTelemetryJson();
            Serial.println("[TELEMETRY] " + tele);
            mqttTransport_publishTelemetry(tele);
        }
    }

    // ── 8. Heartbeat STATE ) ───────────────────────────
    if (stateMachine_getState() == STATE_STANDBY &&
        millis() - lastStateMs > STATE_HEARTBEAT_STANDBY_MS)
    {
        lastStateMs = millis();
        if (mqttTransport_isConnected())
        {
            String stateJson = buildStateJson();
            Serial.println("[STATE] " + stateJson);
            mqttTransport_publishState(stateJson);
        }
    }

    // ── Gestion du portail captif ───────────────────────────────────────────
    captivePortal_loop();

    // ── Boucle de micro-ticks pour réactivité du bouton ─────────────────────
    uint32_t loopEnd = millis() + MAIN_LOOP_DELAY_MS;
    while (millis() < loopEnd)
    {
        handleButtonEvents(); // ← traite long press ET triple clic
        captivePortal_loop(); // ← garde le portail réactif aussi
        delay(10);
    }
}