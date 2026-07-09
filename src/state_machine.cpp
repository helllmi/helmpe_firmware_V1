#include <Arduino.h>
#include "state_machine.h"
#include "led.h"
#include "power_manager.h"
#include "audio.h"
#include "sos_alert.h"
#include "audio_upload.h"
#include "config.h"
#include "voice_trigger.h"

// ============================================================================
//  ÉTAT INTERNE
// ============================================================================
static DeviceState currentState = STATE_OFF;
static uint32_t stateEnteredAt = 0;
static StateChangeCallback stateChangeCb = nullptr;

// Surveillance de la fin d'enregistrement pour déclencher l'upload une seule
// fois par session ACTION (évite de re-déclencher en boucle dans loop()).
static bool wasRecording = false;
static bool uploadEnqueuedForThisAction = false;

// ============================================================================
//  TÂCHE FREERTOS D'UPLOAD (asynchrone, ne bloque pas loop())
// ============================================================================
struct UploadTaskParams {
    char filePath[64];
    char recordId[37];
};

static void uploadTask(void *arg)
{
    UploadTaskParams *params = (UploadTaskParams *)arg;

    Serial.printf("[UPLOAD-TASK] Starting upload for %s\n", params->filePath);

    bool ok = false;
    for (int attempt = 1; attempt <= UPLOAD_MAX_RETRIES; attempt++) {
        Serial.printf("[UPLOAD-TASK] Attempt %d/%d\n", attempt, UPLOAD_MAX_RETRIES);
        ok = audioUpload_send(String(params->filePath), String(params->recordId));
        if (ok) break;
        if (attempt < UPLOAD_MAX_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(UPLOAD_RETRY_DELAY_MS * attempt));
        }
    }

    if (ok) {
        Serial.println("[UPLOAD-TASK] Upload succeeded");
    } else {
        Serial.println("[UPLOAD-TASK] Upload failed after retries");
    }

    sosAlert_reset();
    free(params);
    vTaskDelete(NULL);
}

static void enqueueUpload(const String &filePath, const String &recordId)
{
    UploadTaskParams *params = (UploadTaskParams *)malloc(sizeof(UploadTaskParams));
    if (!params) {
        Serial.println("[FSM] ERROR: cannot allocate upload task params");
        sosAlert_reset();
        return;
    }
    strncpy(params->filePath, filePath.c_str(), sizeof(params->filePath) - 1);
    params->filePath[sizeof(params->filePath) - 1] = '\0';
    strncpy(params->recordId, recordId.c_str(), sizeof(params->recordId) - 1);
    params->recordId[sizeof(params->recordId) - 1] = '\0';

    BaseType_t ok = xTaskCreatePinnedToCore(uploadTask, "audio_upload", 8 * 1024, params, 10, NULL,1);
    if (ok != pdPASS) {
        Serial.println("[FSM] ERROR: cannot create upload task");
        sosAlert_reset();
        free(params);
    }
}

// ============================================================================
//  ACTIONS À CHAQUE TRANSITION
// ============================================================================
// Appelée à l'entrée d'un nouvel état pour faire le setup propre à cet état.
static void onEnterState(DeviceState newState)
{
    switch (newState)
    {
    case STATE_OFF:
        setLED(LED_STARTUP);
        break;

    case STATE_STANDBY:
        setLED(LED_MQTT_OK); // vert sombre = prêt, en veille
        Serial.println("[FSM] >>> Entering STANDBY - waiting for SOS");
        power_enterstandby();
        voiceTrigger_start(); // démarrer l'écoute du mot-clé "SOS"
        audio_stopRecording(); // au cas où on venait d'ACTION
        wasRecording = false;
        uploadEnqueuedForThisAction = false;
        break;

    case STATE_ACTION:
        setLED(LED_ERROR); // rouge = alerte active
        Serial.println("[FSM] >>> Entering ACTION - SOS triggered, alerts enabled");
        power_enteraction();
        voiceTrigger_stop(); // stopper l'écoute du mot-clé "SOS" pendant l'alerte
        uploadEnqueuedForThisAction = false;
        sosAlert_trigger();      // publie l'alerte MQTT (helpme/{id}/sos)
        audio_startRecording();  // démarre l'enregistrement audio
        wasRecording = true;
        break;
    }
}

// ============================================================================
//  TRANSITION CENTRALE
// ============================================================================
// Toute transition d'état passe OBLIGATOIREMENT par cette fonction.
// Elle log, met à jour le timestamp, et appelle onEnterState() pour les actions.
static void transition(DeviceState newState)
{
    if (newState == currentState)
        return; // no-op si déjà dans cet état

    Serial.printf("[FSM] Transition: %s -> %s\n",
                  stateMachine_stateName(currentState),
                  stateMachine_stateName(newState));

    currentState = newState;
    stateEnteredAt = millis();

    onEnterState(newState);
    // Notifier l'extérieur (main.cpp) du changement d'état    
    if (stateChangeCb != nullptr)
    {
        stateChangeCb(newState);
    }
}

// ============================================================================
//  INIT
// ============================================================================
void stateMachine_init()
{
    Serial.println("[FSM] Init - starting in STATE_OFF");
    currentState = STATE_OFF;
    stateEnteredAt = millis();
}

// ============================================================================
//  DISPATCH D'ÉVÉNEMENTS
// ============================================================================
// Les transitions ne sont autorisées que selon l'état courant.
// Un événement reçu dans un état où il n'a pas de sens est IGNORÉ (et loggé).
void stateMachine_dispatch(DeviceEvent evt)
{
    Serial.printf("[FSM] Event received: %d in state %s\n",
                  evt, stateMachine_stateName(currentState));

    switch (currentState)
    {

    // ── État OFF : on n'accepte que EVT_BOOT_OK ─────────────────────────
    case STATE_OFF:
        if (evt == EVT_BOOT_OK)
        {
            transition(STATE_STANDBY);
        }
        else
        {
            Serial.println("[FSM] Event ignored in OFF state");
        }
        break;

    // ── État STANDBY : on n'accepte que EVT_SOS_TRIGGERED ───────────────
    case STATE_STANDBY:
        if (evt == EVT_SOS_TRIGGERED)
        {
            transition(STATE_ACTION);
        }
        else
        {
            Serial.println("[FSM] Event ignored in STANDBY state");
        }
        break;

    // ── État ACTION : on n'accepte que EVT_USER_RESET ───────────────────
    case STATE_ACTION:
        if (evt == EVT_USER_RESET)
        {
            transition(STATE_STANDBY);
        }
        else
        {
            Serial.println("[FSM] Event ignored in ACTION state");
        }
        break;
    }
}

DeviceState stateMachine_getState()
{
    return currentState;
}

uint32_t stateMachine_getStateUptime()
{
    return millis() - stateEnteredAt;
}

const char *stateMachine_stateName(DeviceState s)
{
    switch (s)
    {
    case STATE_OFF:
        return "OFF";
    case STATE_STANDBY:
        return "STANDBY";
    case STATE_ACTION:
        return "ACTION";
    default:
        return "UNKNOWN";
    }
}
void stateMachine_onStateChange(StateChangeCallback cb) {
    stateChangeCb = cb;
}

// ============================================================================
//  TICK — surveille la fin de l'enregistrement audio pendant STATE_ACTION
// ============================================================================
// audio_startRecording() tourne dans sa propre tâche FreeRTOS (voir audio.cpp).
// On détecte ici la transition recording=true → false pour déclencher
// l'upload, sans bloquer loop() ni dupliquer la logique d'enregistrement.
void stateMachine_tick()
{
    if (currentState != STATE_ACTION) return;
    if (uploadEnqueuedForThisAction) return;

    bool isRecordingNow = audio_isRecording();

    // Détection du front descendant : était en train d'enregistrer, ne l'est plus
    if (wasRecording && !isRecordingNow)
    {
        const char *filePath = audio_getLastFilePath();
        const String &recordId = sosAlert_getCurrentRecordId();

        if (filePath[0] != '\0' && recordId.length() > 0)
        {
            Serial.printf("[FSM] Recording finished, enqueueing upload: %s\n", filePath);
            uploadEnqueuedForThisAction = true;
            enqueueUpload(String(filePath), recordId);
        }
        else
        {
            Serial.println("[FSM] Recording finished but no valid filePath/recordId, skip upload");
        }
    }

    wasRecording = isRecordingNow;
}