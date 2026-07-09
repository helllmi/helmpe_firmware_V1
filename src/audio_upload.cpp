#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <esp_task_wdt.h>
#include "audio_upload.h"
#include "config.h"
#include "jwt_storage.h"
#include "serial_comm.h"
#include "mqtt_transport.h"
#include <SD.h>

// ============================================================================
//  CONSTANTES
// ============================================================================
static const char *BOUNDARY = "HeelpMeeFormBoundary7MA4YWxkTrZu0gW";

#define UPLOAD_LTE_WRITE_CHUNK 2048
#define UPLOAD_LTE_ACTION_TIMEOUT_MS 60000
const uint32_t CHUNK_RETRY_BACKOFF_MS = 1500;

// ============================================================================
//  HELPERS COMMUNS
// ============================================================================
static int buildMultipartHeader(char *out, size_t outLen, const String &filename)
{
    return snprintf(out, outLen,
                    "--%s\r\n"
                    "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
                    "Content-Type: audio/wav\r\n"
                    "\r\n",
                    BOUNDARY, filename.c_str());
}

static int buildMultipartFooter(char *out, size_t outLen)
{
    return snprintf(out, outLen, "\r\n--%s--\r\n", BOUNDARY);
}

// ============================================================================
//  CHEMIN WIFI — fichier complet en un seul POST multipart
//  (inchangé — fonctionne déjà avec HTTP 201)
// ============================================================================
static bool uploadViaWifi(const String &filePath, const String &recordId)
{
    Serial.println("[UPLOAD-WIFI] Starting...");

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[UPLOAD-WIFI] WiFi not connected");
        return false;
    }

    File file = SD.open(filePath, FILE_READ);
    if (!file)
    {
        Serial.println("[UPLOAD-WIFI] Cannot open file on SD");
        return false;
    }

    size_t fileSize = file.size();
    if (fileSize <= 44)
    {
        Serial.println("[UPLOAD-WIFI] File too small/empty");
        file.close();
        return false;
    }

    String filename = filePath;
    int slashIdx = filename.lastIndexOf('/');
    if (slashIdx != -1)
        filename = filename.substring(slashIdx + 1);

    char header[256];
    int headerLen = buildMultipartHeader(header, sizeof(header), filename);
    char footer[64];
    int footerLen = buildMultipartFooter(footer, sizeof(footer));
    size_t bodySize = headerLen + fileSize + footerLen;

    String token = jwtStorage_get();

    WiFiClient client;
    if (!client.connect(CLOUD_SERVER_HOST, CLOUD_SERVER_PORT, UPLOAD_RESPONSE_TIMEOUT_MS))
    {
        Serial.println("[UPLOAD-WIFI] TCP connect failed");
        file.close();
        return false;
    }

    client.print("POST ");
    client.print(CLOUD_UPLOAD_PATH);
    client.print(" HTTP/1.1\r\n");
    client.print("Host: ");
    client.print(CLOUD_SERVER_HOST);
    if (CLOUD_SERVER_PORT != 80 && CLOUD_SERVER_PORT != 443)
    {
        client.print(":");
        client.print(CLOUD_SERVER_PORT);
    }
    client.print("\r\n");
    client.print("Authorization: Bearer ");
    client.print(token);
    client.print("\r\n");
    client.print("X-Device-Id: ");
    client.print(DEVICE_ID);
    client.print("\r\n");
    client.print("X-Record-Id: ");
    client.print(recordId);
    client.print("\r\n");
    client.print("X-Chunk-Index: 0\r\n");
    client.print("X-Chunk-Total: 1\r\n");
    client.print("Content-Type: multipart/form-data; boundary=");
    client.print(BOUNDARY);
    client.print("\r\n");
    client.print("Content-Length: ");
    client.print(bodySize);
    client.print("\r\n");
    client.print("Connection: close\r\n\r\n");

    client.write((const uint8_t *)header, headerLen);
    uint8_t buf[512];
    size_t remaining = fileSize;
    while (remaining > 0)
    {
        size_t toRead = (remaining > sizeof(buf)) ? sizeof(buf) : remaining;
        size_t bytesRead = file.read(buf, toRead);
        if (bytesRead == 0)
            break;
        client.write(buf, bytesRead);
        remaining -= bytesRead;
    }
    file.close();
    client.write((const uint8_t *)footer, footerLen);
    client.flush();

    unsigned long deadline = millis() + UPLOAD_RESPONSE_TIMEOUT_MS;
    char response[256];
    int respIdx = 0;
    while (millis() < deadline && client.connected() && respIdx < (int)sizeof(response) - 1)
    {
        while (client.available() && respIdx < (int)sizeof(response) - 1)
        {
            response[respIdx++] = (char)client.read();
            deadline = millis() + 500;
        }
        delay(0);
    }
    response[respIdx] = '\0';
    client.stop();

    if (respIdx == 0)
    {
        Serial.println("[UPLOAD-WIFI] No response");
        return false;
    }

    int httpCode = 0;
    char *sp1 = strchr(response, ' ');
    if (sp1)
    {
        char *sp2 = strchr(sp1 + 1, ' ');
        if (sp2)
        {
            *sp2 = '\0';
            httpCode = atoi(sp1 + 1);
        }
    }
    Serial.printf("[UPLOAD-WIFI] HTTP response: %d\n", httpCode);
    return (httpCode >= 200 && httpCode < 300);
}

// ============================================================================
//  CHEMIN LTE — helpers AT HTTP
// ============================================================================
static void clearSerialBuffer()
{
    while (ss.available())
        ss.read();
}

static bool waitForDownloadPrompt(uint32_t timeoutMs)
{
    String acc = "";
    uint32_t deadline = millis() + timeoutMs;
    while (millis() < deadline)
    {
        if (ss.available())
        {
            char c = (char)ss.read();
            acc += c;
            if (acc.indexOf("DOWNLOAD") != -1)
                return true;
        }
    }
    Serial.printf("[LTE-HTTP] DOWNLOAD prompt timeout, got: %s\n", acc.c_str());
    return false;
}

// ============================================================================
//  lteHttpPost() — helper générique pour un POST AT+HTTP*
//  Gère le cycle complet : HTTPINIT → HTTPPARA → HTTPDATA → HTTPACTION →
//                          HTTPREAD → HTTPTERM
//  Retourne true si le cycle AT s'est terminé, httpCodeOut = code HTTP,
//  responseBody = body JSON de la réponse serveur.
// ============================================================================
static bool lteHttpPost(const String &url,
                        const String &contentType,
                        const String &userdata,
                        const uint8_t *bodyData, size_t bodyLen,
                        int &httpCodeOut, String &responseBody)
{
    httpCodeOut = 0;
    responseBody = "";

    // 1) HTTPINIT
    clearSerialBuffer();
    if (!SentMessage("AT+HTTPINIT", 5000))
    {
        SentMessage("AT+HTTPTERM", 3000);
        delay(500);
        if (!SentMessage("AT+HTTPINIT", 5000))
        {
            Serial.println("[LTE-HTTP] HTTPINIT failed");
            return false;
        }
    }
    delay(150);

    // 2) HTTPPARA URL
    {
        String cmd = "AT+HTTPPARA=\"URL\",\"" + url + "\"";
        if (!SentMessage(cmd.c_str(), 3000))
        {
            Serial.println("[LTE-HTTP] HTTPPARA URL failed");
            SentMessage("AT+HTTPTERM", 3000);
            return false;
        }
    }

    // 3) HTTPPARA CONTENT (si fourni)
    if (contentType.length() > 0)
    {
        String cmd = "AT+HTTPPARA=\"CONTENT\",\"" + contentType + "\"";
        if (!SentMessage(cmd.c_str(), 3000))
        {
            Serial.println("[LTE-HTTP] HTTPPARA CONTENT failed");
            SentMessage("AT+HTTPTERM", 3000);
            return false;
        }
    }
    delay(100);

    // 4) HTTPPARA USERDATA (headers custom, max 256 chars SIM7670G)
    if (userdata.length() > 0)
    {
        String fullCmd = String("AT+HTTPPARA=\"USERDATA\",\"") + userdata + "\"";
        Serial.printf("[LTE-HTTP] USERDATA CMD (%d chars): %s\n",
                      fullCmd.length(), fullCmd.c_str());
        clearSerialBuffer();
        ss.print("AT+HTTPPARA=\"USERDATA\",\"");
        ss.print(userdata);
        ss.println("\"");

        String udResp = "";
        uint32_t t = millis();
        while (millis() - t < 3000)
        {
            while (ss.available())
                udResp += (char)ss.read();
            if (udResp.indexOf("OK") != -1 || udResp.indexOf("ERROR") != -1)
                break;
            delay(10);
        }
        if (udResp.indexOf("ERROR") != -1)
        {
            Serial.printf("[LTE-HTTP] USERDATA failed: %s\n", udResp.c_str());
            SentMessage("AT+HTTPTERM", 3000);
            return false;
        }
    }
    delay(100);
    Serial.printf("[LTE-HTTP] USERDATA length=%d\n", userdata.length());

    // 5) HTTPDATA (body — si fourni)
    if (bodyData != NULL && bodyLen > 0)
    {
        char dataCmd[64];
        snprintf(dataCmd, sizeof(dataCmd), "AT+HTTPDATA=%u,30", (unsigned)bodyLen);
        ss.println(dataCmd);

        if (!waitForDownloadPrompt(15000))
        {
            SentMessage("AT+HTTPTERM", 3000);
            return false;
        }

        size_t sent = 0;
        while (sent < bodyLen)
        {
            size_t toSend = min((size_t)UPLOAD_LTE_WRITE_CHUNK, bodyLen - sent);
            ss.write(bodyData + sent, toSend);
            sent += toSend;
            delay(5);
        }
        ss.flush();
        delay(20);

        // Attendre OK
        unsigned long start = millis();
        String dataResp = "";
        while (millis() - start < 10000)
        {
            while (ss.available())
                dataResp += (char)ss.read();
            if (dataResp.indexOf("OK") != -1)
                break;
            delay(10);
        }
        if (dataResp.indexOf("OK") == -1)
        {
            Serial.printf("[LTE-HTTP] HTTPDATA no OK: %s\n", dataResp.c_str());
            SentMessage("AT+HTTPTERM", 3000);
            return false;
        }
    }

    // 6) HTTPACTION=1 (POST)
    ss.println("AT+HTTPACTION=1");
    String actionResp = "";
    unsigned long _start = millis();
    while (millis() - _start < UPLOAD_LTE_ACTION_TIMEOUT_MS)
    {
        while (ss.available())
            actionResp += (char)ss.read();
        if (actionResp.indexOf("+HTTPACTION:") != -1)
            break;
        esp_task_wdt_reset();
        delay(10);
    }
    Serial.printf("[LTE-HTTP] HTTPACTION: %s\n", actionResp.c_str());

    int idx = actionResp.indexOf("+HTTPACTION: 1,");
    if (idx == -1)
    {
        Serial.println("[LTE-HTTP] No HTTPACTION response");
        SentMessage("AT+HTTPTERM", 5000);
        return false;
    }
    httpCodeOut = actionResp.substring(idx + 15).toInt();

    // 7) Lire le body de la réponse
    String readResp = SentMessageResponse("AT+HTTPREAD=0,512", 5000);
    int jsonStart = readResp.indexOf('{');
    int jsonEnd = readResp.lastIndexOf('}');
    if (jsonStart != -1 && jsonEnd != -1)
    {
        responseBody = readResp.substring(jsonStart, jsonEnd + 1);
    }
    Serial.printf("[LTE-HTTP] HTTP %d | body: %s\n", httpCodeOut, responseBody.c_str());

    // 8) Cleanup
    SentMessage("AT+HTTPTERM", 5000);
    return true;
}

// ============================================================================
//  Extraction JSON simple (sans ArduinoJson — économie mémoire)
// ============================================================================
static String jsonExtractString(const String &json, const char *key)
{
    // Cherche "key":"value" ou "key": "value"
    String search = String("\"") + key + "\"";
    int keyIdx = json.indexOf(search);
    if (keyIdx == -1)
        return "";

    int colon = json.indexOf(':', keyIdx + search.length());
    if (colon == -1)
        return "";

    // Trouver le premier guillemet après le ':'
    int q1 = json.indexOf('"', colon + 1);
    if (q1 == -1)
        return "";
    int q2 = json.indexOf('"', q1 + 1);
    if (q2 == -1)
        return "";

    return json.substring(q1 + 1, q2);
}

static int jsonExtractInt(const String &json, const char *key)
{
    String search = String("\"") + key + "\"";
    int keyIdx = json.indexOf(search);
    if (keyIdx == -1)
        return 0;

    int colon = json.indexOf(':', keyIdx + search.length());
    if (colon == -1)
        return 0;

    String val = json.substring(colon + 1);
    val.trim();
    return val.toInt();
}
static bool uploadChunkedViaWifi(const String &filePath, const String &recordId)
{
    Serial.println("[UPLOAD-WIFI-CHUNK] Starting chunked upload (3 steps)...");

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[UPLOAD-WIFI-CHUNK] WiFi not connected");
        return false;
    }

    // Lire le fichier
    File file = SD.open(filePath, FILE_READ);
    if (!file)
    {
        Serial.println("[UPLOAD-WIFI-CHUNK] Cannot open file on SD");
        return false;
    }

    size_t fileSize = file.size();
    if (fileSize <= 44)
    {
        Serial.println("[UPLOAD-WIFI-CHUNK] File too small");
        file.close();
        return false;
    }

    uint8_t *fileBuf = (uint8_t *)malloc(fileSize);
    if (!fileBuf)
    {
        Serial.println("[UPLOAD-WIFI-CHUNK] Cannot allocate buffer");
        file.close();
        return false;
    }

    size_t bytesRead = file.read(fileBuf, fileSize);
    file.close();

    if (bytesRead != fileSize)
    {
        Serial.printf("[UPLOAD-WIFI-CHUNK] SD read short (%u/%u)\n",
                      (unsigned)bytesRead, (unsigned)fileSize);
        free(fileBuf);
        return false;
    }

    Serial.printf("[UPLOAD-WIFI-CHUNK] File: %u bytes\n", (unsigned)fileSize);

    String token = jwtStorage_get();
    String baseHost = String(CLOUD_SERVER_HOST);
    uint16_t basePort = CLOUD_SERVER_PORT;

    int httpCode = 0;
    String respBody = "";

    // ═══════════════════════════════════════════════════════════
    //  ÉTAPE 1 : POST /audio/upload/init
    // ═══════════════════════════════════════════════════════════
    Serial.println("[UPLOAD-WIFI-CHUNK] === STEP 1/3 : INIT ===");

    {
        WiFiClient client;
        if (!client.connect(baseHost.c_str(), basePort, 10000))
        {
            Serial.println("[UPLOAD-WIFI-CHUNK] TCP connect failed for init");
            free(fileBuf);
            return false;
        }

        String initUrl = String("/audio/upload/init?device_id=") + DEVICE_ID + "&record_id=" + recordId + "&file_size=" + String((unsigned)fileSize);
        client.print("POST ");
        client.print(initUrl);
        client.print(" HTTP/1.1\r\n");
        client.print("Host: ");
        client.print(baseHost);
        if (basePort != 80 && basePort != 443)
        {
            client.print(":");
            client.print(basePort);
        }
        client.print("\r\n");
        client.print("Authorization: Bearer ");
        client.print(token);
        client.print("\r\n");
        client.print("Content-Length: 0\r\n");
        client.print("Connection: close\r\n\r\n");
        client.flush();

        unsigned long deadline = millis() + 15000;
        String rawResp = "";
        while (millis() < deadline && client.connected())
        {
            while (client.available())
            {
                rawResp += (char)client.read();
                deadline = millis() + 1000;
            }
            delay(10);
        }
        client.stop();

        httpCode = 0;
        respBody = "";
        char *sp1 = strchr(rawResp.c_str(), ' ');
        if (sp1)
            httpCode = atoi(sp1 + 1);
        int js = rawResp.indexOf('{');
        int je = rawResp.lastIndexOf('}');
        if (js != -1 && je != -1)
            respBody = rawResp.substring(js, je + 1);

        Serial.printf("[UPLOAD-WIFI-CHUNK] Init HTTP %d | %s\n", httpCode, respBody.c_str());

        if (httpCode < 200 || httpCode >= 300)
        {
            Serial.println("[UPLOAD-WIFI-CHUNK] Init failed");
            free(fileBuf);
            return false;
        }
    }

    // Parser upload_id et max_chunk_size
    String uploadId = jsonExtractString(respBody, "upload_id");
    int maxChunkSize = jsonExtractInt(respBody, "max_chunk_size");
    if (maxChunkSize <= 0)
        maxChunkSize = 65536;

    if (uploadId.length() == 0)
    {
        Serial.println("[UPLOAD-WIFI-CHUNK] No upload_id in response");
        free(fileBuf);
        return false;
    }

    Serial.printf("[UPLOAD-WIFI-CHUNK] upload_id=%s max_chunk=%d\n",
                  uploadId.c_str(), maxChunkSize);

    // ═══════════════════════════════════════════════════════════
    //  ÉTAPE 2 : POST /audio/upload/{upload_id}/chunk (boucle)
    // ═══════════════════════════════════════════════════════════
    Serial.println("[UPLOAD-WIFI-CHUNK] === STEP 2/3 : CHUNKS ===");

    size_t totalChunks = (fileSize + maxChunkSize - 1) / maxChunkSize;
    size_t offset = 0;

    for (size_t i = 0; i < totalChunks; i++)
    {
        size_t chunkSize = min((size_t)maxChunkSize, fileSize - offset);

        Serial.printf("[UPLOAD-WIFI-CHUNK] Chunk %u/%u (%u bytes)\n",
                      (unsigned)(i + 1), (unsigned)totalChunks, (unsigned)chunkSize);

        String chunkPath = String("/audio/upload/") + uploadId + "/chunk";

        WiFiClient client;
        if (!client.connect(baseHost.c_str(), basePort, 10000))
        {
            Serial.println("[UPLOAD-WIFI-CHUNK] TCP connect failed for chunk");
            free(fileBuf);
            return false;
        }

        String chunkUrl = chunkPath;
        client.print("POST ");
        client.print(chunkUrl);
        client.print(" HTTP/1.1\r\n");
        client.print("Host: ");
        client.print(baseHost);
        if (basePort != 80 && basePort != 443)
        {
            client.print(":");
            client.print(basePort);
        }
        client.print("\r\n");
        client.print("Authorization: Bearer ");
        client.print(token);
        client.print("\r\n");
        client.print("X-Chunk-Index: ");
        client.print(i);
        client.print("\r\n");
        client.print("X-Total-Chunks: ");
        client.print(totalChunks);
        client.print("\r\n");
        client.print("Content-Type: application/octet-stream\r\n");
        client.print("Content-Length: ");
        client.print(chunkSize);
        client.print("\r\n");
        client.print("Connection: close\r\n\r\n");

        // Envoyer le chunk
        size_t sent = 0;
        while (sent < chunkSize)
        {
            size_t toSend = min((size_t)512, chunkSize - sent);
            client.write(fileBuf + offset + sent, toSend);
            sent += toSend;
        }
        client.flush();

        // Lire la réponse
        unsigned long deadline = millis() + 15000;
        String rawResp = "";
        while (millis() < deadline && client.connected())
        {
            while (client.available())
            {
                rawResp += (char)client.read();
                deadline = millis() + 1000;
            }
            delay(10);
        }
        client.stop();

        httpCode = 0;
        respBody = "";
        char *sp1 = strchr(rawResp.c_str(), ' ');
        if (sp1)
            httpCode = atoi(sp1 + 1);
        int js = rawResp.indexOf('{');
        int je = rawResp.lastIndexOf('}');
        if (js != -1 && je != -1)
            respBody = rawResp.substring(js, je + 1);

        Serial.printf("[UPLOAD-WIFI-CHUNK] Chunk %u HTTP %d | %s\n",
                      (unsigned)i, httpCode, respBody.c_str());

        if (httpCode < 200 || httpCode >= 300)
        {
            Serial.printf("[UPLOAD-WIFI-CHUNK] Chunk %u failed\n", (unsigned)i);
            free(fileBuf);
            return false;
        }

        offset += chunkSize;
    }

    free(fileBuf);
    Serial.printf("[UPLOAD-WIFI-CHUNK] All %u chunks sent\n", (unsigned)totalChunks);

    // ═══════════════════════════════════════════════════════════
    //  ÉTAPE 3 : POST /audio/upload/{upload_id}/complete
    // ═══════════════════════════════════════════════════════════
    Serial.println("[UPLOAD-WIFI-CHUNK] === STEP 3/3 : COMPLETE ===");

    {
        String completePath = String("/audio/upload/") + uploadId + "/complete";

        WiFiClient client;
        if (!client.connect(baseHost.c_str(), basePort, 10000))
        {
            Serial.println("[UPLOAD-WIFI-CHUNK] TCP connect failed for complete");
            return false;
        }

        client.print("POST ");
        client.print(completePath);
        client.print(" HTTP/1.1\r\n");
        client.print("Host: ");
        client.print(baseHost);
        if (basePort != 80 && basePort != 443)
        {
            client.print(":");
            client.print(basePort);
        }
        client.print("\r\n");
        client.print("Authorization: Bearer ");
        client.print(token);
        client.print("\r\n");
        client.print("Content-Length: 0\r\n");
        client.print("Connection: close\r\n\r\n");
        client.flush();

        unsigned long deadline = millis() + 15000;
        String rawResp = "";
        while (millis() < deadline && client.connected())
        {
            while (client.available())
            {
                rawResp += (char)client.read();
                deadline = millis() + 1000;
            }
            delay(10);
        }
        client.stop();

        httpCode = 0;
        respBody = "";
        char *sp1 = strchr(rawResp.c_str(), ' ');
        if (sp1)
            httpCode = atoi(sp1 + 1);
        int js = rawResp.indexOf('{');
        int je = rawResp.lastIndexOf('}');
        if (js != -1 && je != -1)
            respBody = rawResp.substring(js, je + 1);

        Serial.printf("[UPLOAD-WIFI-CHUNK] Complete HTTP %d | %s\n",
                      httpCode, respBody.c_str());
    }

    bool success = (httpCode >= 200 && httpCode < 300);
    Serial.printf("[UPLOAD-WIFI-CHUNK] Upload %s\n", success ? "OK ✓" : "FAILED ✗");
    return success;
}

// ============================================================================
//  CHEMIN LTE — upload chunké en 3 étapes
//
//  1. POST /audio/upload/init
//     Headers: Authorization, X-Device-Id, X-Record-Id, X-File-Size
//     → { "upload_id": "...", "max_chunk_size": 65536 }
//
//  2. Boucle: POST /audio/upload/{upload_id}/chunk
//     Headers: Authorization, X-Chunk-Index, X-Total-Chunks
//     Body: raw bytes (max max_chunk_size)
//     → { "chunks_received": N, "status": "in_progress" }
//
//  3. POST /audio/upload/{upload_id}/complete
//     Headers: Authorization
//     → { "audio_id": "...", "audio_url": "...", "status": "received" }
//
//  CONTRAINTE SIM7670G : USERDATA ≤ 256 chars.
//  Authorization: Bearer <token> = ~227 chars.
//  Les autres headers (X-Device-Id, X-Record-Id, etc.) sont passés en
//  query params dans l'URL pour contourner la limite.
// ============================================================================
static bool uploadViaLte(const String &filePath, const String &recordId)
{
    Serial.println("[UPLOAD-LTE] Starting chunked upload (3 steps)...");

    // Verrouiller le UART modem
    if (!modemMutex_take(10000))
    {
        Serial.println("[UPLOAD-LTE] Cannot acquire modem mutex");
        return false;
    }

    // ── Lire le fichier WAV depuis la SD ────────────────────────────────
    File file = SD.open(filePath, FILE_READ);
    if (!file)
    {
        Serial.println("[UPLOAD-LTE] Cannot open file on SD");

        modemMutex_give();
        return false;
    }

    size_t fileSize = file.size();
    if (fileSize <= 44)
    {
        Serial.println("[UPLOAD-LTE] File too small");
        file.close();
        modemMutex_give();
        return false;
    }

    uint8_t *fileBuf = (uint8_t *)malloc(fileSize);
    if (!fileBuf)
    {
        Serial.println("[UPLOAD-LTE] Cannot allocate buffer");
        file.close();
        modemMutex_give();
        return false;
    }

    size_t bytesRead = file.read(fileBuf, fileSize);
    file.close();

    if (bytesRead != fileSize)
    {
        Serial.printf("[UPLOAD-LTE] SD read short (%u/%u)\\n",
                      (unsigned)bytesRead, (unsigned)fileSize);
        free(fileBuf);
        
        modemMutex_give();
        return false;
    }

    Serial.printf("[UPLOAD-LTE] File: %u bytes, RIFF=%c%c%c%c\n",
                  (unsigned)fileSize, fileBuf[0], fileBuf[1], fileBuf[2], fileBuf[3]);

    String token = jwtStorage_get();

    Serial.printf("[UPLOAD-LTE] FULL TOKEN: %s\n", token.c_str());
    Serial.printf("[UPLOAD-LTE] Token length: %d\n", token.length());
    String baseUrl = String("http://") + CLOUD_SERVER_HOST + ":" +
                     String(CLOUD_SERVER_PORT);

    // USERDATA : Authorization seule (~227 chars, dans la limite 256)
    String authUserdata = String("Authorization: Bearer ") + token;

    int httpCode = 0;
    String respBody = "";
    Serial.printf("[UPLOAD-LTE] USERDATA à envoyer (%d chars): %s\\r\\nX-Device-Id: %s\n",
                  (int)(authUserdata.length() + 4 + 10 + strlen(DEVICE_ID)),
                  authUserdata.c_str(), DEVICE_ID);

    // ═════════════════════════════════════════════════════════════════════
    //  ÉTAPE 1 : POST /audio/upload/init
    // ═════════════════════════════════════════════════════════════════════
    Serial.println("[UPLOAD-LTE] === STEP 1/3 : INIT ===");

    // X-Device-Id, X-Record-Id, X-File-Size en query params (USERDATA trop petit)
    // X-Device-Id dans USERDATA (le serveur l'exige comme header)
    // X-Record-Id et X-File-Size en query params (USERDATA trop petit pour tout)
    String initUserdata = authUserdata;

    String initUrl = baseUrl + "/audio/upload/init"
                               "?device_id=" +
                     String(DEVICE_ID) +
                     "&record_id=" + recordId +
                     "&file_size=" + String(fileSize);

    if (!lteHttpPost(initUrl, "application/json", initUserdata,
                     NULL, 0, httpCode, respBody))
    {
        Serial.println("[UPLOAD-LTE] Init request failed");
        free(fileBuf);
        modemMutex_give();
        return false;
    }

    if (httpCode < 200 || httpCode >= 300)
    {
        Serial.printf("[UPLOAD-LTE] Init failed HTTP %d: %s\n", httpCode, respBody.c_str());
        free(fileBuf);
        modemMutex_give();
        return false;
    }

    // Parser upload_id et max_chunk_size
    String uploadId = jsonExtractString(respBody, "upload_id");
    int maxChunkSize = jsonExtractInt(respBody, "max_chunk_size");
    if (maxChunkSize <= 0)
        maxChunkSize = 16384; // fallback réduit pour LTE
    if (maxChunkSize > 16384)
        maxChunkSize = 16384; // cap LTE : buffer SIM7670G limité

    if (uploadId.length() == 0)
    {
        Serial.println("[UPLOAD-LTE] No upload_id in response");
        free(fileBuf);
        modemMutex_give();
        return false;
    }

    Serial.printf("[UPLOAD-LTE] upload_id=%s max_chunk=%d\n",
                  uploadId.c_str(), maxChunkSize);

    // ═════════════════════════════════════════════════════════════════════
    //  ÉTAPE 2 : POST /audio/upload/{upload_id}/chunk (boucle)
    // ═════════════════════════════════════════════════════════════════════
    Serial.println("[UPLOAD-LTE] === STEP 2/3 : CHUNKS ===");

    size_t totalChunks = (fileSize + maxChunkSize - 1) / maxChunkSize;
    size_t offset = 0;

    for (size_t i = 0; i < totalChunks; i++)
    {
        size_t chunkSize = min((size_t)maxChunkSize, fileSize - offset);

        // X-Chunk-Index et X-Total-Chunks en query params (USERDATA ne supporte pas \r\n)
        String chunkUrl = baseUrl + "/audio/upload/" + uploadId + "/chunk"
                                                                  "?chunk_index=" +
                          String((unsigned)i) +
                          "&total_chunks=" + String((unsigned)totalChunks);
        String chunkUserdata = initUserdata; // Authorization only

        bool chunkOk = false;
        for (int attempt = 1; attempt <= UPLOAD_CHUNK_MAX_RETRIES   && !chunkOk; attempt++)
        {
            Serial.printf("[UPLOAD-LTE] Chunk %u/%u (%u bytes) [try %d/%d]\n",
                          (unsigned)(i + 1), (unsigned)totalChunks,
                          (unsigned)chunkSize, attempt, UPLOAD_CHUNK_MAX_RETRIES);

            if (lteHttpPost(chunkUrl, "application/octet-stream", chunkUserdata,
                            fileBuf + offset, chunkSize, httpCode, respBody) &&
                httpCode >= 200 && httpCode < 300)
            {
                chunkOk = true;
                break;
            }

            // 706/707 = transitoire réseau : on laisse la radio récupérer et on rejoue CE chunk
            Serial.printf("[UPLOAD-LTE] Chunk %u HTTP %d — retry dans %lu ms\n",
                          (unsigned)i, httpCode, (unsigned long)CHUNK_RETRY_BACKOFF_MS);
            delay(CHUNK_RETRY_BACKOFF_MS);
        }

        if (!chunkOk)
        {
            Serial.printf("[UPLOAD-LTE] Chunk %u définitivement échoué après %d essais\n",
                          (unsigned)i, UPLOAD_CHUNK_MAX_RETRIES);
            free(fileBuf);
            modemMutex_give();
            return false;
        }

        offset += chunkSize;
    }

    free(fileBuf);
    Serial.printf("[UPLOAD-LTE] All %u chunks sent\n", (unsigned)totalChunks);

    // ═════════════════════════════════════════════════════════════════════
    //  ÉTAPE 3 : POST /audio/upload/{upload_id}/complete
    // ═════════════════════════════════════════════════════════════════════
    Serial.println("[UPLOAD-LTE] === STEP 3/3 : COMPLETE ===");

    String completeUrl = baseUrl + "/audio/upload/" + uploadId + "/complete";

    if (!lteHttpPost(completeUrl, "application/json", authUserdata,
                     NULL, 0, httpCode, respBody))
    {
        Serial.println("[UPLOAD-LTE] Complete request failed");
        modemMutex_give();
        return false;
    }

    bool success = (httpCode >= 200 && httpCode < 300);
    Serial.printf("[UPLOAD-LTE] Complete HTTP %d: %s → %s\n",
                  httpCode, respBody.c_str(), success ? "OK" : "FAILED");
    modemMutex_give();
    return success;
}

// ============================================================================
//  API PUBLIQUE
// ============================================================================
bool audioUpload_send(const String &filePath, const String &recordId)
{
    Serial.printf("[UPLOAD] Sending %s (record_id=%s) via %s\n",
                  filePath.c_str(), recordId.c_str(), mqttTransport_typeName());

    if (mqttTransport_isWifi())
        return uploadChunkedViaWifi(filePath, recordId);
    else
        return uploadViaLte(filePath, recordId);
}