#include <Arduino.h>
#include <Update.h>
#include <mbedtls/sha256.h>
#include "ota_lte.h"
#include "ota.h"          // shaToHex(), debugAllPartitions()
#include "serial_comm.h"  // SentMessage, SentMessageResponse, SentMessageAsync
#include "watchdog.h"     // feedWatchdog(), markOtaPendingValidation()
#include "led.h"
#include "mqtt_transport.h" // mqttTransport_publishOtaStatus()

// ============================================================================
//  CONSTANTES
// ============================================================================
// Taille max d'un chunk AT+HTTPREAD selon la datasheet SIM7670G
#define OTA_LTE_CHUNK_SIZE  1460

// Timeout AT+HTTPACTION=0 : le modem télécharge le fichier entier avant de répondre.
// Pour un firmware ~1MB sur LTE Cat-M1 (~375 kbps downlink), on compte large.
#define OTA_LTE_GET_TIMEOUT_MS  120000   // 2 minutes

// Timeout AT+HTTPREAD par chunk
#define OTA_LTE_READ_TIMEOUT_MS  10000

// ============================================================================
//  HELPERS AT
// ============================================================================

// Configure un paramètre HTTP : AT+HTTPPARA="<tag>","<value>"
static bool httpPara(const char* tag, const char* value)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "AT+HTTPPARA=\"%s\",\"%s\"", tag, value);
    return SentMessage(cmd, 3000);
}

// Lit le content-length renvoyé dans l'URC AT+HTTPACTION.
// Format URC : +HTTPACTION: 0,200,<size>
// Retourne 0 si non trouvé.
static size_t parseHttpActionSize(const String& resp)
{
    int idx = resp.indexOf("+HTTPACTION: 0,200,");
    if (idx == -1) return 0;

    String tail = resp.substring(idx + 19); // après "+HTTPACTION: 0,200,"
    tail.trim();
    return (size_t)tail.toInt();
}

// ============================================================================
//  CLEANUP
// ============================================================================
void otaLte_cleanup()
{
    Serial.println("[OTA-LTE] Cleanup (HTTPTERM)");
    SentMessage("AT+HTTPTERM", 5000);
    delay(500);
}

// ============================================================================
//  PROCESSUS OTA LTE COMPLET
// ============================================================================
bool performOTA_LTE(const String& url,
                    const String& expectedSha256,
                    size_t        expectedSize)
{
    Serial.println("\n========== OTA LTE UPDATE START ==========");
    Serial.printf("[OTA-LTE] URL: %s\n", url.c_str());
    Serial.printf("[OTA-LTE] Expected SHA256: %s\n", expectedSha256.c_str());
    Serial.printf("[OTA-LTE] Expected size: %u bytes\n", expectedSize);

    // ── Garde-fou taille minimum (hérité du chemin WiFi) ──────────────────
    if (expectedSize < 500 * 1024) {
        Serial.printf("[OTA-LTE] REJECTED: size %u < 500KB\n", expectedSize);
        mqttTransport_publishOtaStatus("{\"state\":\"failed\",\"reason\":\"size_invalid\"}");
        return false;
    }

    setLED(LED_OTA_DOWNLOAD);
    mqttTransport_publishOtaStatus("{\"state\":\"downloading\"}");

    // ── 1. Initialiser le service HTTP du modem ───────────────────────────
    Serial.println("[OTA-LTE] HTTPINIT...");
    if (!SentMessage("AT+HTTPINIT", 5000)) {
        // Peut déjà être init — tenter un cleanup puis réessayer
        Serial.println("[OTA-LTE] HTTPINIT failed, trying cleanup+retry");
        otaLte_cleanup();
        delay(1000);
        if (!SentMessage("AT+HTTPINIT", 5000)) {
            Serial.println("[OTA-LTE] HTTPINIT retry failed");
            mqttTransport_publishOtaStatus("{\"state\":\"failed\",\"reason\":\"httpinit\"}");
            return false;
        }
    }
    Serial.println("[OTA-LTE] HTTPINIT OK");
    delay(200);

    // ── 2. Configurer l'URL ───────────────────────────────────────────────
    if (!httpPara("URL", url.c_str())) {
        Serial.println("[OTA-LTE] HTTPPARA URL failed");
        otaLte_cleanup();
        mqttTransport_publishOtaStatus("{\"state\":\"failed\",\"reason\":\"httppara\"}");
        return false;
    }

    // Content-type pour la réponse binaire
    httpPara("ACCEPT", "application/octet-stream");
    delay(200);

    // ── 3. Lancer le GET (le modem télécharge tout en interne) ────────────
    Serial.println("[OTA-LTE] AT+HTTPACTION=0 — downloading firmware (may take ~60s)...");
    String getResp = SentMessageAsync("AT+HTTPACTION=0", "+HTTPACTION:", OTA_LTE_GET_TIMEOUT_MS);
    Serial.printf("[OTA-LTE] HTTPACTION resp: %s\n", getResp.c_str());

    // Vérifier le code HTTP 200
    if (getResp.indexOf("+HTTPACTION: 0,200") == -1) {
        Serial.println("[OTA-LTE] HTTPACTION failed or non-200");
        otaLte_cleanup();
        mqttTransport_publishOtaStatus("{\"state\":\"failed\",\"reason\":\"httpaction\"}");
        return false;
    }

    size_t contentLength = parseHttpActionSize(getResp);
    if (contentLength == 0) {
        // Taille absente dans l'URC — on fait confiance à expectedSize
        Serial.printf("[OTA-LTE] Content-length not in URC, using expectedSize=%u\n",
                      expectedSize);
        contentLength = expectedSize;
    } else {
        Serial.printf("[OTA-LTE] Content-length from modem: %u bytes\n", contentLength);
    }

    // Garde-fou : taille cohérente avec ce qu'annonce le backend
    if (contentLength < 500 * 1024) {
        Serial.printf("[OTA-LTE] REJECTED: modem reports %u bytes < 500KB\n", contentLength);
        otaLte_cleanup();
        mqttTransport_publishOtaStatus("{\"state\":\"failed\",\"reason\":\"size_mismatch\"}");
        return false;
    }

    // ── 4. Init Update (partition OTA) ────────────────────────────────────
    if (!Update.begin(contentLength)) {
        Serial.printf("[OTA-LTE] Update.begin failed: %s\n", Update.errorString());
        otaLte_cleanup();
        mqttTransport_publishOtaStatus("{\"state\":\"failed\",\"reason\":\"begin\"}");
        return false;
    }

    // ── 5. Lecture chunk par chunk via AT+HTTPREAD ────────────────────────
    mbedtls_sha256_context shaCtx;
    mbedtls_sha256_init(&shaCtx);
    mbedtls_sha256_starts(&shaCtx, 0);

    size_t totalRead    = 0;
    uint32_t lastPct    = 0;
    uint8_t  buf[OTA_LTE_CHUNK_SIZE];

    Serial.printf("[OTA-LTE] Reading %u bytes in chunks of %d...\n",
                  contentLength, OTA_LTE_CHUNK_SIZE);

    while (totalRead < contentLength)
    {
        feedWatchdog();

        size_t remaining = contentLength - totalRead;
        size_t toRead    = min((size_t)OTA_LTE_CHUNK_SIZE, remaining);

        // Envoyer AT+HTTPREAD=0,<n>
        char readCmd[32];
        snprintf(readCmd, sizeof(readCmd), "AT+HTTPREAD=0,%u", (unsigned)toRead);

        // La réponse contient : +HTTPREAD: 0,<n>\r\n<binary data>\r\nOK
        // On attend le début de la réponse via SentMessageResponse,
        // puis on lit les octets bruts depuis ss directement.
        String header = SentMessageResponse(readCmd, OTA_LTE_READ_TIMEOUT_MS);

        // Parser la taille confirmée par le modem
        int hIdx = header.indexOf("+HTTPREAD: 0,");
        if (hIdx == -1) {
            Serial.printf("[OTA-LTE] HTTPREAD unexpected response: %s\n", header.c_str());
            Update.abort();
            otaLte_cleanup();
            mqttTransport_publishOtaStatus("{\"state\":\"failed\",\"reason\":\"httpread\"}");
            return false;
        }

        String sizeStr = header.substring(hIdx + 13);
        size_t chunkSize = (size_t)sizeStr.toInt();

        if (chunkSize == 0 || chunkSize > sizeof(buf)) {
            Serial.printf("[OTA-LTE] Invalid chunk size: %u\n", chunkSize);
            Update.abort();
            otaLte_cleanup();
            mqttTransport_publishOtaStatus("{\"state\":\"failed\",\"reason\":\"chunk_size\"}");
            return false;
        }

        // Lire les octets binaires bruts depuis UART1 (ss)
        size_t bytesRead = 0;
        uint32_t deadline = millis() + OTA_LTE_READ_TIMEOUT_MS;
        while (bytesRead < chunkSize && millis() < deadline) {
            if (ss.available()) {
                buf[bytesRead++] = (uint8_t)ss.read();
            }
        }

        if (bytesRead != chunkSize) {
            Serial.printf("[OTA-LTE] Incomplete chunk: got %u / %u\n",
                          bytesRead, chunkSize);
            Update.abort();
            otaLte_cleanup();
            mqttTransport_publishOtaStatus("{\"state\":\"failed\",\"reason\":\"incomplete_chunk\"}");
            return false;
        }

        // Écrire dans la partition OTA
        if (Update.write(buf, bytesRead) != bytesRead) {
            Serial.printf("[OTA-LTE] Update.write failed: %s\n", Update.errorString());
            Update.abort();
            otaLte_cleanup();
            mqttTransport_publishOtaStatus("{\"state\":\"failed\",\"reason\":\"write\"}");
            return false;
        }

        // SHA256 incrémental
        mbedtls_sha256_update(&shaCtx, buf, bytesRead);
        totalRead += bytesRead;

        // Progress log tous les 10%
        uint32_t pct = (totalRead * 100) / contentLength;
        if (pct >= lastPct + 10) {
            Serial.printf("[OTA-LTE] Progress: %u%% (%u/%u bytes)\n",
                          pct, totalRead, contentLength);
            lastPct = pct;
        }
    }

    // ── 6. Cleanup modem HTTP ─────────────────────────────────────────────
    otaLte_cleanup();

    // ── 7. Vérification SHA256 ────────────────────────────────────────────
    uint8_t computedHash[32];
    mbedtls_sha256_finish(&shaCtx, computedHash);
    mbedtls_sha256_free(&shaCtx);

    String computed;
    shaToHex(computedHash, computed);

    if (computed != expectedSha256) {
        Serial.println("[OTA-LTE] SHA256 MISMATCH - aborting");
        Serial.printf("[OTA-LTE] Expected : %s\n", expectedSha256.c_str());
        Serial.printf("[OTA-LTE] Computed : %s\n", computed.c_str());
        Update.abort();
        mqttTransport_publishOtaStatus("{\"state\":\"failed\",\"reason\":\"sha256\"}");
        setLED(LED_ERROR);
        return false;
    }
    Serial.println("[OTA-LTE] SHA256 verified OK");

    // ── 8. Finalisation ───────────────────────────────────────────────────
    setLED(LED_OTA_INSTALL);
    if (!Update.end(true) || !Update.isFinished()) {
        Serial.printf("[OTA-LTE] Update.end failed: %s\n", Update.errorString());
        mqttTransport_publishOtaStatus("{\"state\":\"failed\",\"reason\":\"end\"}");
        setLED(LED_ERROR);
        return false;
    }

    Serial.println("[OTA-LTE] Success - rebooting in 3s");
    mqttTransport_publishOtaStatus("{\"state\":\"installing\"}");

    markOtaPendingValidation();
    delay(3000);
    ESP.restart();
    return true; // jamais atteint
}