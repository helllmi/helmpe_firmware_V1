#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include "watchdog.h"
#include "mqtt_client.h"
#include "led.h"
#include "mqtt_transport.h"
#include "boot_mode.h"

// ============================================================================
//  ÉTAT
// ============================================================================
static Preferences preferences;

static bool otaObservationActive = false;
static uint32_t otaObservationStartMs = 0;
static uint32_t otaLastCheckMs = 0;
static uint8_t otaConsecutiveFailures = 0;

static const char *NVS_NAMESPACE = "ota";
static const char *NVS_KEY_PENDING = "pending";

// ============================================================================
//  WATCHDOG HARDWARE
// ============================================================================
void setupWatchdog()
{
    Serial.printf("[WDT] Init watchdog (timeout=%us)\n", WDT_TIMEOUT_S);
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = WDT_TIMEOUT_S * 1000,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    esp_task_wdt_reconfigure(&wdt_config);
#else
    esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
    esp_task_wdt_add(NULL);
}

void feedWatchdog()
{
    esp_task_wdt_reset();
}

// ============================================================================
//  NVS - DRAPEAU "OTA PENDING"
// ============================================================================
void markOtaPendingValidation()
{
    preferences.begin(NVS_NAMESPACE, false);
    preferences.putBool(NVS_KEY_PENDING, true);
    preferences.end();
    Serial.println("[NVS] Marked OTA as pending validation");
}

bool isOtaPendingValidation()
{
    preferences.begin(NVS_NAMESPACE, true);
    bool pending = preferences.getBool(NVS_KEY_PENDING, false);
    preferences.end();
    return pending;
}

void confirmOtaSuccess()
{
    preferences.begin(NVS_NAMESPACE, false);
    preferences.remove(NVS_KEY_PENDING);
    preferences.end();
    Serial.println("[NVS] OTA validation flag cleared");
}

// ============================================================================
//  SELF-TEST APRÈS OTA - démarre la fenêtre d'observation si nécessaire
// ============================================================================
void runSelfTestAfterOTA()
{
    Serial.println("\n[SELFTEST] ========== START ==========");

    bool pending = isOtaPendingValidation();
    Serial.printf("[SELFTEST] OTA pending validation: %s\n",
                  pending ? "YES" : "NO");

    if (!pending)
    {
        Serial.println("[SELFTEST] Regular boot - no observation needed");
        Serial.println("[SELFTEST] ========== END ==========\n");
        return;
    }

    Serial.println("[SELFTEST] >>> POST-OTA BOOT DETECTED <<<");
    Serial.printf("[SELFTEST] Observation period: %u seconds\n",
                  OTA_OBSERVATION_PERIOD_MS / 1000);
    Serial.printf("[SELFTEST] Health check interval: %u seconds\n",
                  OTA_HEALTH_CHECK_INTERVAL_MS / 1000);
    Serial.printf("[SELFTEST] Rollback after %u consecutive failures\n",
                  OTA_MAX_FAILURES);

    otaObservationActive = true;
    otaObservationStartMs = millis();
    otaLastCheckMs = millis();
    otaConsecutiveFailures = 0;

    Serial.println("[SELFTEST] ========== END ==========\n");
}

bool isOtaObserving()
{
    return otaObservationActive;
}

// ============================================================================
//  HEALTH CHECK - critères : Wi-Fi + MQTT
// ============================================================================
static bool runHealthCheck()
{
    bool mqttOK = mqttTransport_isConnected();
 
    if (bootMode_isForceWifi())
    {
        bool wifiOK = (WiFi.status() == WL_CONNECTED);
        Serial.printf("[HEALTH] Transport=WiFi | WiFi=%s, MQTT=%s\n",
                      wifiOK ? "OK" : "FAIL",
                      mqttOK ? "OK" : "FAIL");
        return wifiOK && mqttOK;
    }
    else
    {
        // LTE : on ne vérifie pas WiFi.status() — le modem gère le réseau
        Serial.printf("[HEALTH] Transport=LTE | MQTT=%s\n",
                      mqttOK ? "OK" : "FAIL");
        return mqttOK;
    }
}
// ============================================================================
//  TICK - à appeler depuis loop()
// ============================================================================
void otaObservationTick()
{
    if (!otaObservationActive)
        return;

    uint32_t now = millis();
    uint32_t elapsed = now - otaObservationStartMs;

    if (now - otaLastCheckMs >= OTA_HEALTH_CHECK_INTERVAL_MS)
    {
        otaLastCheckMs = now;

        uint32_t remainingS = (OTA_OBSERVATION_PERIOD_MS - elapsed) / 1000;
        Serial.printf("\n[WATCHDOG] Health check (%u sec remaining)\n", remainingS);

        bool healthy = runHealthCheck();

        if (healthy)
        {
            if (otaConsecutiveFailures > 0)
            {
                Serial.printf("[WATCHDOG] Recovered (was %u failures)\n",
                              otaConsecutiveFailures);
            }
            otaConsecutiveFailures = 0;
        }
        else
        {
            otaConsecutiveFailures++;
            Serial.printf("[WATCHDOG] Failure %u/%u\n",
                          otaConsecutiveFailures, OTA_MAX_FAILURES);

            if (otaConsecutiveFailures >= OTA_MAX_FAILURES)
            {
                Serial.println("\n[WATCHDOG] ========== ROLLBACK TRIGGERED ==========");
                Serial.printf("[WATCHDOG] %u consecutive failures - rolling back\n",
                              OTA_MAX_FAILURES);
                otaObservationActive = false;
                manualRollback();
                return; // jamais atteint
            }
        }
    }

    // Fin de période : si on traînait des échecs → rollback de sécurité
    if (elapsed >= OTA_OBSERVATION_PERIOD_MS)
    {
        if (otaConsecutiveFailures > 0)
        {
            Serial.println("\n[WATCHDOG] ========== PERIOD ENDED WITH FAILURES ==========");
            Serial.printf("[WATCHDOG] %u failures at end of period - rolling back\n",
                          otaConsecutiveFailures);
            otaObservationActive = false;
            manualRollback();
            return;
        }

        Serial.println("\n[WATCHDOG] ========== OBSERVATION COMPLETE ==========");
        Serial.printf("[WATCHDOG] Stable for %u seconds - confirming OTA\n",
                      OTA_OBSERVATION_PERIOD_MS / 1000);
        confirmOtaSuccess();
        otaObservationActive = false;
    }
}

// ============================================================================
//  ROLLBACK MANUEL
// ============================================================================
void manualRollback()
{
    Serial.println("\n[ROLLBACK] ========== MANUAL ROLLBACK START ==========");
    setLED(LED_ROLLBACK);

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *previous = esp_ota_get_next_update_partition(NULL);

    Serial.printf("[ROLLBACK] Currently running on: %s @ 0x%x\n",
                  running->label, running->address);

    if (previous == NULL)
    {
        Serial.println("[ROLLBACK] No previous partition - reboot only");
        delay(2000);
        ESP.restart();
        return;
    }

    Serial.printf("[ROLLBACK] Switching to: %s @ 0x%x\n",
                  previous->label, previous->address);

    esp_err_t err = esp_ota_set_boot_partition(previous);
    if (err != ESP_OK)
    {
        Serial.printf("[ROLLBACK] set_boot_partition FAILED: %s\n",
                      esp_err_to_name(err));
        delay(2000);
        ESP.restart();
        return;
    }

    // On garde le drapeau pending=true volontairement.
    // Après le reboot sur l'ancienne partition, si Wi-Fi+MQTT OK pendant la
    // période d'observation, le drapeau sera nettoyé par confirmOtaSuccess().
    Serial.println("[ROLLBACK] Rebooting in 2 seconds...");
    Serial.println("[ROLLBACK] ========== END ==========\n");
    delay(2000);
    ESP.restart();
}
