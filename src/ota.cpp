#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>
#include "ota.h"
#include "mqtt_client.h"
#include "watchdog.h"
#include "led.h"
#include "Ota_lte.h"
#include "mqtt_transport.h" 
// ============================================================================
//  ÉTAT
// ============================================================================
bool otaInProgress = false;

// ============================================================================
//  UTILITAIRES
// ============================================================================
void shaToHex(const uint8_t *hash, String &out)
{
    out = "";
    for (int i = 0; i < 32; i++)
    {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", hash[i]);
        out += buf;
    }
}

void debugAllPartitions()
{
    Serial.println("\n========== PARTITIONS DEBUG ==========");
    const esp_partition_t *running = esp_ota_get_running_partition();
    Serial.printf("Running on: %s @ 0x%x\n", running->label, running->address);

    esp_partition_iterator_t it = esp_partition_find(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);

    while (it != NULL)
    {
        const esp_partition_t *p = esp_partition_get(it);
        esp_ota_img_states_t state;
        esp_err_t err = esp_ota_get_state_partition(p, &state);

        const char *stateStr = "EMPTY";
        if (err == ESP_OK)
        {
            switch (state)
            {
            case ESP_OTA_IMG_NEW:
                stateStr = "NEW";
                break;
            case ESP_OTA_IMG_PENDING_VERIFY:
                stateStr = "PENDING_VERIFY";
                break;
            case ESP_OTA_IMG_VALID:
                stateStr = "VALID";
                break;
            case ESP_OTA_IMG_INVALID:
                stateStr = "INVALID";
                break;
            case ESP_OTA_IMG_ABORTED:
                stateStr = "ABORTED";
                break;
            case ESP_OTA_IMG_UNDEFINED:
                stateStr = "UNDEFINED";
                break;
            }
        }

        Serial.printf("  %s @ 0x%x: state=%s%s\n",
                      p->label, p->address, stateStr,
                      (p == running) ? "  <-- RUNNING" : "");

        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);
    Serial.println("======================================\n");
}
//  ROUTAGE : WiFi ou LTE selon le transport actif
// ============================================================================
bool performOTA(const String &url,
                const String &expectedSha256,
                size_t        expectedSize)
{
    // Garde-fou taille minimum (500 KB)
    if (expectedSize < 500 * 1024) {
        Serial.printf("[OTA] REJECTED: expectedSize %u < 500KB\n", expectedSize);
        mqttTransport_publishOtaStatus("{\"state\":\"failed\",\"reason\":\"size_invalid\"}");
        return false;
    }
 
    // Routage selon transport actif
    bool useLTE = strcmp(mqttTransport_typeName(), "LTE")== 0;
    Serial.printf("[OTA] Transport: %s\n", useLTE ? "LTE" : "WiFi");
 
    if (useLTE) {
        return performOTA_LTE(url, expectedSha256, expectedSize);
    } else {
        return performOTA_WIFI(url, expectedSha256, expectedSize);
    }
}

// ============================================================================
//  PROCESSUS OTA COMPLET
// ============================================================================
bool performOTA_WIFI(const String &url,
                const String &expectedSha256,
                size_t expectedSize)
{

    Serial.println("\n========== OTA UPDATE START ==========");
    Serial.printf("[OTA] URL: %s\n", url.c_str());
    Serial.printf("[OTA] Expected SHA256: %s\n", expectedSha256.c_str());

    setLED(LED_OTA_DOWNLOAD);
    mqttPublishOtaStatus("{\"state\":\"downloading\"}");

    HTTPClient http;
    http.begin(url);
    http.setTimeout(30000);

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK)
    {
        Serial.printf("[OTA] HTTP error: %d\n", httpCode);
        mqttPublishOtaStatus("{\"state\":\"failed\",\"reason\":\"http\"}");
        setLED(LED_ERROR);
        http.end();
        return false;
    }

    int contentLength = http.getSize();
    if (contentLength <= 0)
    {
        Serial.println("[OTA] Invalid content length");
        http.end();
        setLED(LED_ERROR);
        return false;
    }

    if (!Update.begin(contentLength))
    {
        Serial.printf("[OTA] Update.begin failed: %s\n", Update.errorString());
        mqttPublishOtaStatus("{\"state\":\"failed\",\"reason\":\"begin\"}");
        http.end();
        setLED(LED_ERROR);
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    mbedtls_sha256_context shaCtx;
    mbedtls_sha256_init(&shaCtx);
    mbedtls_sha256_starts(&shaCtx, 0);

    uint8_t buf[1024];
    size_t totalWritten = 0;
    uint32_t lastProgress = 0;

    while (http.connected() && (totalWritten < (size_t)contentLength))
    {
        size_t available = stream->available();
        if (available > 0)
        {
            int read = stream->readBytes(buf, min(available, sizeof(buf)));
            if (read > 0)
            {
                if (Update.write(buf, read) != (size_t)read)
                {
                    Update.abort();
                    http.end();
                    mqttPublishOtaStatus("{\"state\":\"failed\",\"reason\":\"write\"}");
                    setLED(LED_ERROR);
                    return false;
                }
                mbedtls_sha256_update(&shaCtx, buf, read);
                totalWritten += read;

                uint32_t pct = (totalWritten * 100) / contentLength;
                if (pct >= lastProgress + 10)
                {
                    Serial.printf("[OTA] Progress: %u%%\n", pct);
                    lastProgress = pct;
                }
            }
        }
        feedWatchdog();
        delay(1);
    }
    http.end();

    // Vérification SHA256
    uint8_t computedHash[32];
    mbedtls_sha256_finish(&shaCtx, computedHash);
    mbedtls_sha256_free(&shaCtx);

    String computed;
    shaToHex(computedHash, computed);

    if (computed != expectedSha256)
    {
        Serial.println("[OTA] SHA256 MISMATCH - aborting");
        Serial.printf("[OTA] Computed: %s\n", computed.c_str());
        Update.abort();
        mqttPublishOtaStatus("{\"state\":\"failed\",\"reason\":\"sha256\"}");
        setLED(LED_ERROR);
        return false;
    }
    Serial.println("[OTA] SHA256 verified OK");

    // Finalisation
    setLED(LED_OTA_INSTALL);
    if (!Update.end(true) || !Update.isFinished())
    {
        Serial.printf("[OTA] Update.end failed: %s\n", Update.errorString());
        setLED(LED_ERROR);
        return false;
    }

    Serial.println("[OTA] Success - rebooting in 3s");
    mqttPublishOtaStatus("{\"state\":\"installing\"}");

    // CRITIQUE : marque que la prochaine boot devra être validée par le watchdog
    markOtaPendingValidation();

    delay(3000);
    ESP.restart();
    return true; // ne sera jamais exécuté
}
