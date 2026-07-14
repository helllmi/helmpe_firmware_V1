#include "audio_stream.h"
#include <WiFi.h>
#include <WebSocketsClient.h>
#include "config.h"
#include "audio.h"           // audio_streamReadChunk, audio_isRecording
#include "voice_trigger.h"   // voiceTrigger_start/stop/isRunning
#include "state_machine.h"   // stateMachine_getState, STATE_STANDBY
#include "jwt_storage.h"     
#include "mqtt_client.h"      // mqttPublishStreamState

// ============================================================================
//  CONFIG STREAM (à déplacer dans config.h plus tard si tu veux)
// ============================================================================
#define ASTREAM_HOST          "helpmee.nacloud.tn"   // IP du serveur audio — À ADAPTER
#define ASTREAM_PORT          3000
#define ASTREAM_PATH_FMT      "/audio/ws/device/%s"
#define ASTREAM_CHUNK_SAMPLES 512               // samples int16 par envoi (~32 ms @ 16k)
#define ASTREAM_MAX_MS        60000             // cap dur : 60 s
#define ASTREAM_TASK_PRIO     2
#define ASTREAM_TASK_STACK    8192

// ============================================================================
//  ÉTAT
// ============================================================================
enum AStreamPhase { AS_IDLE, AS_CONNECTING, AS_STREAMING, AS_STOPPING };

static WebSocketsClient ws;
static TaskHandle_t     taskHandle    = NULL;
static volatile AStreamPhase phase    = AS_IDLE;
static volatile bool    reqStart      = false;
static volatile bool    reqStop       = false;
static volatile bool    wsConnected   = false;
static bool             headerSent    = false;
static unsigned long    streamStartMs = 0;
static char             streamId[40]  = {0};

// ============================================================================
//  HEADER WAV (44 o, int16 mono @ AUDIO_SAMPLE_RATE) — envoyé une fois
// ============================================================================
static void buildWavHeader(uint8_t *h)
{
    uint32_t sr = AUDIO_SAMPLE_RATE;
    uint16_t ch = 1, bits = 16;
    uint16_t blockAlign = ch * (bits / 8);
    uint32_t byteRate   = sr * blockAlign;

    memcpy(h, "RIFF", 4);
    h[4]=0xFF; h[5]=0xFF; h[6]=0xFF; h[7]=0xFF;   // taille inconnue (flux)
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    h[16]=16; h[17]=0; h[18]=0; h[19]=0;          // taille sous-chunk fmt
    h[20]=1;  h[21]=0;                            // PCM
    h[22]=ch & 0xFF; h[23]=(ch >> 8) & 0xFF;
    h[24]=sr & 0xFF; h[25]=(sr>>8)&0xFF; h[26]=(sr>>16)&0xFF; h[27]=(sr>>24)&0xFF;
    h[28]=byteRate&0xFF; h[29]=(byteRate>>8)&0xFF; h[30]=(byteRate>>16)&0xFF; h[31]=(byteRate>>24)&0xFF;
    h[32]=blockAlign&0xFF; h[33]=(blockAlign>>8)&0xFF;
    h[34]=bits&0xFF; h[35]=(bits>>8)&0xFF;
    memcpy(h + 36, "data", 4);
    h[40]=0xFF; h[41]=0xFF; h[42]=0xFF; h[43]=0xFF;
}

// ============================================================================
//  WEBSOCKET (tourne dans la tâche stream)
// ============================================================================
static void wsEvent(WStype_t type, uint8_t *payload, size_t len)
{
    switch (type)
    {
        case WStype_CONNECTED:
            wsConnected   = true;
            headerSent    = false;
            streamStartMs = millis();
            Serial.println("[STREAM] WS connected");
            break;
        case WStype_DISCONNECTED:
            wsConnected = false;
            Serial.println("[STREAM] WS disconnected");
            break;
        case WStype_ERROR:
            wsConnected = false;
            Serial.println("[STREAM] WS error");
            break;
        default:
            break;
    }
}

static void wsConnect()
{
    char path[64];
    snprintf(path, sizeof(path), ASTREAM_PATH_FMT, DEVICE_ID);
    String url = String("ws://") + ASTREAM_HOST + ":" + ASTREAM_PORT + path +
                 "?token="  + jwtStorage_get() +
                 "&device=" + DEVICE_ID +
                 "&sr=" + String(AUDIO_SAMPLE_RATE) + "&ch=1&bits=16";
    Serial.printf("[STREAM] Connecting %s\n", url.c_str());
    ws.begin(ASTREAM_HOST, ASTREAM_PORT, url.c_str(), "arduino");
}

static void sendChunk()
{
    int16_t pcm[ASTREAM_CHUNK_SAMPLES];
    size_t got = audio_streamReadChunk(pcm, ASTREAM_CHUNK_SAMPLES);
    if (got == 0) return;

    if (!headerSent)
    {
        uint8_t h[44];
        buildWavHeader(h);
        ws.sendBIN(h, 44);
        headerSent = true;
        Serial.println("[STREAM] WAV header sent");
    }
    ws.sendBIN((uint8_t *)pcm, got * sizeof(int16_t));
}

// ============================================================================
//  TÂCHE STREAM
// ============================================================================
static void streamTask(void *)
{
    ws.onEvent(wsEvent);

    for (;;)
    {
        switch (phase)
        {
        case AS_IDLE:
            if (reqStart)
            {
                reqStart = false;
                // Gardes Option A : uniquement en STANDBY, hors enregistrement, WiFi up
                if (stateMachine_getState() != STATE_STANDBY ||
                    audio_isRecording() ||
                    WiFi.status() != WL_CONNECTED)
                {
                    Serial.println("[STREAM] Refused (not STANDBY / recording / no WiFi)");
                    phase = AS_STOPPING;   // cleanup + éventuel resume vt
                }
                else
                {
                    if (voiceTrigger_isRunning()) voiceTrigger_stop();  // libère l'I2S
                    vTaskDelay(pdMS_TO_TICKS(50));
                    wsConnect();
                    phase = AS_CONNECTING;
                }
            }
            else vTaskDelay(pdMS_TO_TICKS(100));
            break;

        case AS_CONNECTING:
            ws.loop();
            if (wsConnected) phase = AS_STREAMING;
            else if (reqStop || stateMachine_getState() != STATE_STANDBY ||
                     WiFi.status() != WL_CONNECTED)
                phase = AS_STOPPING;
            vTaskDelay(pdMS_TO_TICKS(20));
            break;

        case AS_STREAMING:
            ws.loop();
            if (reqStop ||
                (millis() - streamStartMs) > ASTREAM_MAX_MS ||
                stateMachine_getState() != STATE_STANDBY ||
                audio_isRecording() ||
                WiFi.status() != WL_CONNECTED ||
                !wsConnected)
            {
                phase = AS_STOPPING;
            }
            else sendChunk();
            vTaskDelay(pdMS_TO_TICKS(20));
            break;

        case AS_STOPPING:
        {
            // Le SOS a-t-il repris la main ? (ACTION ou enregistrement en cours)
            bool sosTookOver = (stateMachine_getState() != STATE_STANDBY) || audio_isRecording();

            if (wsConnected) { ws.disconnect(); ws.loop(); }
            wsConnected = false;

            // Reprendre voice_trigger UNIQUEMENT si on reste en STANDBY.
            // Si un SOS a pris la main, c'est le chemin d'enregistrement qui
            // possède l'I2S et gère voice_trigger.
            if (!sosTookOver && !voiceTrigger_isRunning()) voiceTrigger_start();

            reqStop = false; reqStart = false; headerSent = false;
            Serial.println("[STREAM] Stopped");
            phase = AS_IDLE;
            vTaskDelay(pdMS_TO_TICKS(20));
            break;
        }
        }
    }
}

// ============================================================================
//  API PUBLIQUE
// ============================================================================
void audioStream_init()
{
    if (taskHandle) return;
    xTaskCreate(streamTask, "audio_stream", ASTREAM_TASK_STACK, NULL,
                ASTREAM_TASK_PRIO, &taskHandle);
    Serial.println("[STREAM] Task created");
}

void audioStream_requestStart(const char *sid)
{
    strncpy(streamId, sid ? sid : "", sizeof(streamId) - 1);
    streamId[sizeof(streamId) - 1] = '\0';
    reqStop  = false;
    reqStart = true;
}

void audioStream_requestStop() { reqStop = true; }

bool audioStream_isActive() { return phase == AS_STREAMING || phase == AS_CONNECTING; }

// Appelé dans loop() (main task). Publie l'état MQTT sur changement de phase.
void audioStream_tick()
{
    static AStreamPhase published = AS_IDLE;
    if (phase == published) return;
    published = phase;

    const char *s = (phase == AS_STREAMING)  ? "streaming"
                  : (phase == AS_CONNECTING) ? "connecting"
                                             : "idle";
    Serial.printf("[STREAM] state -> %s\n", s);
    mqttPublishStreamState(s, streamId);
}