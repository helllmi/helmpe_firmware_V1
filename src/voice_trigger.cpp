#include <Arduino.h>
#include <driver/i2s.h>
#include "voice_trigger.h"
#include "state_machine.h"
#include "config.h"
#include "i2s_resource.h"

#include <Alert-helpmee_inferencing.h>

// ============================================================================
//  ÉTAT INTERNE
// ============================================================================
static volatile bool vtRunning = false;
static volatile bool vtStopReq = false;
static volatile bool vtCaptureAlive = false;
static TaskHandle_t vtTaskHandle = NULL;
static TaskHandle_t vtCaptureHandle = NULL;

typedef struct
{
    int16_t *buffers[2];
    uint8_t buf_select;
    uint8_t buf_ready;
    uint32_t buf_count;
    uint32_t n_samples;
} vt_inference_t;

static vt_inference_t vtInference;

// Buffer PCM 16-bit partagé capture → callback
#define I2S_READ_BYTES (EI_CLASSIFIER_SLICE_SIZE * 4)
static int16_t vtSampleBuffer[EI_CLASSIFIER_SLICE_SIZE];

// ============================================================================
//  AGC + NOISE GATE
// ============================================================================
static void i2s_to_pcm_agc(int16_t *out, const uint8_t *in32, uint32_t bytes_in)
{
    static float gain = 4.0f;
    const float GAIN_MAX = 12.0f;
    const float GAIN_MIN = 1.0f;
    const float TARGET = 8000.0f;
    const float ATTACK = 0.01f;
    const float RELEASE = 0.001f;
    const float GATE_OPEN = 300.0f;
    const float GATE_CLOSE = 150.0f;
    static bool gate_open = false;
    static int hold_samples = 0;
    const int HOLD_MAX = 80;

    float peak = 0;
    for (uint32_t i = 0; i < bytes_in; i += 4)
    {
        int32_t r = (int32_t)(((uint32_t)in32[i + 3] << 24) | ((uint32_t)in32[i + 2] << 16) |
                              ((uint32_t)in32[i + 1] << 8) | ((uint32_t)in32[i + 0]));
        float s = fabsf((float)(int16_t)(r >> 8));
        if (s > peak)
            peak = s;
    }

    if (peak > GATE_OPEN)
    {
        gate_open = true;
        hold_samples = HOLD_MAX;
    }
    else if (peak < GATE_CLOSE)
    {
        if (hold_samples > 0)
            hold_samples--;
        else
            gate_open = false;
    }

    if (gate_open && peak > 0.1f)
    {
        float d = TARGET / peak;
        if (d < gain)
            gain += (d - gain) * ATTACK;
        else
            gain += (d - gain) * RELEASE;
        if (gain > GAIN_MAX)
            gain = GAIN_MAX;
        if (gain < GAIN_MIN)
            gain = GAIN_MIN;
    }

    uint32_t n_samples = bytes_in / 4;
    for (uint32_t i = 0, j = 0; i < bytes_in && j < n_samples; i += 4, j++)
    {
        int32_t r = (int32_t)(((uint32_t)in32[i + 3] << 24) | ((uint32_t)in32[i + 2] << 16) |
                              ((uint32_t)in32[i + 1] << 8) | ((uint32_t)in32[i + 0]));
        int16_t s = (int16_t)(r >> 14);
        int32_t a = gate_open ? (int32_t)((float)s * gain) : 0;
        if (a > 32767)
            a = 32767;
        if (a < -32768)
            a = -32768;
        out[j] = (int16_t)a;
    }
}

// ============================================================================
//  CALLBACK
// ============================================================================
static void vt_audio_callback(uint32_t n_samples_read)
{
    for (uint32_t i = 0; i < n_samples_read; i++)
    {
        vtInference.buffers[vtInference.buf_select][vtInference.buf_count++] =
            vtSampleBuffer[i];

        if (vtInference.buf_count >= vtInference.n_samples)
        {
            vtInference.buf_select ^= 1;
            vtInference.buf_count = 0;
            vtInference.buf_ready = 1;
        }
    }
}

// ============================================================================
//  TÂCHE DE CAPTURE I2S
//  Buffer 32-bit alloué sur le HEAP (16KB trop grand pour la stack)
// ============================================================================
static void vt_capture_task(void *arg)
{
    if (!i2sResource_take())
    {
        Serial.println("[VT] Cannot take I2S mutex, capture task aborting");
        vtStopReq = true;
        vtCaptureAlive = false;
        vTaskDelete(NULL);
        return;
    }
    Serial.println("[VT] I2S mutex taken (capture)");

    // Allouer le buffer 32-bit brut sur le heap (EI_CLASSIFIER_SLICE_SIZE * 4 bytes)
    uint8_t *local_raw = (uint8_t *)malloc(I2S_READ_BYTES);
    if (!local_raw)
    {
        Serial.println("[VT] ERROR: cannot allocate capture buffer");
        i2sResource_give();
        vtCaptureAlive = false;
        vTaskDelete(NULL);
        return;
    }

    vtCaptureAlive = true;
    vtCaptureHandle = xTaskGetCurrentTaskHandle();

    size_t bytes_read;

    while (!vtStopReq)
    {
        esp_err_t err = i2s_read(I2S_NUM_0, local_raw, I2S_READ_BYTES,
                                 &bytes_read, pdMS_TO_TICKS(100));
        if (err != ESP_OK || bytes_read == 0)
            continue;

        i2s_to_pcm_agc(vtSampleBuffer, local_raw, bytes_read);

        uint32_t samples_read = bytes_read / 4;
        vt_audio_callback(samples_read);
    }

    free(local_raw);

    i2sResource_give();
    Serial.println("[VT] I2S mutex released (capture)");

    vtCaptureAlive = false;
    vtCaptureHandle = NULL;

    vTaskDelete(NULL);
}

// ============================================================================
//  GET DATA — callback Edge Impulse
// ============================================================================
static int vt_get_data(size_t offset, size_t length, float *out_ptr)
{
    int16_t *buf = vtInference.buffers[vtInference.buf_select ^ 1];
    if (!buf)
    {
        memset(out_ptr, 0, length * sizeof(float));
        return 0;
    }
    for (size_t i = 0; i < length; i++)
    {
        out_ptr[i] = (float)vtInference.buffers[vtInference.buf_select ^ 1]
                                               [offset + i] /
                     32768.0f;
    }
    return 0;
}

// ============================================================================
//  TÂCHE D'INFÉRENCE
// ============================================================================
static void vt_inference_task(void *arg)
{
    Serial.println("[VT] Inference task started");

    vtInference.buffers[0] = (int16_t *)malloc(EI_CLASSIFIER_SLICE_SIZE * sizeof(int16_t));
    vtInference.buffers[1] = (int16_t *)malloc(EI_CLASSIFIER_SLICE_SIZE * sizeof(int16_t));

    if (!vtInference.buffers[0] || !vtInference.buffers[1])
    {
        Serial.println("[VT] ERROR: cannot allocate inference buffers");
        free(vtInference.buffers[0]);
        free(vtInference.buffers[1]);
        vtRunning = false;
        vTaskDelete(NULL);
        return;
    }

    vtInference.buf_select = 0;
    vtInference.buf_count = 0;
    vtInference.n_samples = EI_CLASSIFIER_SLICE_SIZE;
    vtInference.buf_ready = 0;

    run_classifier_init();

    xTaskCreate(vt_capture_task, "vt_capture", 4 * 1024, NULL, 12, NULL);

    Serial.printf("[VT] Listening for 'Sos' (threshold=%.2f)...\n",
                  VOICE_TRIGGER_THRESHOLD);

    int print_results = -(EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW);

    while (!vtStopReq)
    {
        while (vtInference.buf_ready == 0 && !vtStopReq)
        {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        if (vtStopReq)
            break;

        vtInference.buf_ready = 0;

        signal_t signal;
        signal.total_length = EI_CLASSIFIER_SLICE_SIZE;
        signal.get_data = &vt_get_data;

        ei_impulse_result_t result = {0};
        EI_IMPULSE_ERROR err = run_classifier_continuous(&signal, &result, false);

        if (err != EI_IMPULSE_OK)
        {
            Serial.printf("[VT] Classifier error: %d\n", err);
            continue;
        }

        if (++print_results >= (EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW))
        {
            print_results = 0;

            float sos_score = 0.0f;
            float noise_score = 0.0f;

            for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++)
            {
                float score = result.classification[ix].value;
                const char *label = result.classification[ix].label;

                Serial.printf("[VT] %s: %.2f\n", label, score);

                if (strcmp(label, "Sos") == 0)
                    sos_score = score;
                if (strcmp(label, "noise") == 0)
                    noise_score = score;
            }

            // Filtre noise
            if (noise_score > 0.80f)
            {
                Serial.println("[VT] Noise filtered");
                continue;
            }

            // Déclenchement SOS
            if (sos_score >= VOICE_TRIGGER_THRESHOLD &&
                stateMachine_getState() == STATE_STANDBY)
            {
                Serial.printf("[VT] *** SOS DETECTED (score=%.2f) ***\n", sos_score);
                vtStopReq = true;
                vtRunning = false;
                stateMachine_dispatch(EVT_SOS_TRIGGERED);
            }
        }
    }

    // Attendre arrêt propre de capture avant de libérer les buffers
    uint32_t t = millis();
    while (vtCaptureAlive && millis() - t < 1000)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (vtCaptureAlive)
    {
        Serial.println("[VT] WARNING: capture task did not stop in time");
    }

    free(vtInference.buffers[0]);
    free(vtInference.buffers[1]);
    vtInference.buffers[0] = nullptr;
    vtInference.buffers[1] = nullptr;

    vtRunning = false;
    vtTaskHandle = NULL;
    Serial.println("[VT] Inference task stopped");
    vTaskDelete(NULL);
}

// ============================================================================
//  API PUBLIQUE
// ============================================================================
void voiceTrigger_start()
{
    if (vtRunning)
    {
        Serial.println("[VT] Already running");
        return;
    }
    // Reset complet de l'état interne
    vtInference.buffers[0] = nullptr;
    vtInference.buffers[1] = nullptr;
    vtInference.buf_select = 0;
    vtInference.buf_count = 0;
    vtInference.buf_ready = 0;

    vtStopReq = false;
    vtRunning = true;
    vtCaptureAlive = false;
    vtCaptureHandle = NULL;

    BaseType_t ok = xTaskCreate(vt_inference_task, "vt_infer",
                                16 * 1024, NULL, 5, &vtTaskHandle);
    if (ok != pdPASS)
    {
        Serial.println("[VT] ERROR: cannot create inference task");
        vtRunning = false;
    }
    else
    {
        Serial.println("[VT] Started");
    }
}

void voiceTrigger_stop()
{
    if (!vtRunning)
        return;

    Serial.println("[VT] Stop requested");
    vtStopReq = true;

    uint32_t deadline = millis() + 3000;
    while (vtRunning && millis() < deadline)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (vtRunning)
    {
        // Ne jamais force-delete — juste forcer les flags
        vtTaskHandle = NULL;
        vtRunning = false;
        Serial.println("[VT] Force stopped");
    }
    else
    {
        Serial.println("[VT] Stopped cleanly");
    }
    vtStopReq = false;
}

bool voiceTrigger_isRunning()
{
    return vtRunning;
}