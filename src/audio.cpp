#include <Arduino.h>
#include "audio.h"
#include "config.h"
#include <driver/i2s.h>
#include <SD.h>
#include <SPI.h>
#include "lte.h"
#include "i2s_resource.h"

// ============================================================================
//  ÉTAT INTERNE
// ============================================================================
static volatile bool recording = false;
static volatile bool stopRequested = false;
static uint32_t lastSeq = 0;
static char lastFilePath[64] = "";
static TaskHandle_t recordTaskHandle = NULL;

// ============================================================================
//  I2S INIT — lecture native 32-bit (micro INMP441), comm format standard I2S
// ============================================================================
static void i2s_init()
{
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = AUDIO_SAMPLE_RATE,
        .bits_per_sample = (i2s_bits_per_sample_t)AUDIO_SAMPLE_BITS, // 32-bit
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_STAND_I2S),
        .intr_alloc_flags = 0,
        .dma_buf_count = 8,
        .dma_buf_len = 1024,
        .use_apll = 1};
    i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);

    const i2s_pin_config_t pins = {
        .bck_io_num = PIN_I2S_SCK,
        .ws_io_num = PIN_I2S_WS,
        .data_out_num = -1,
        .data_in_num = PIN_I2S_SD};
    i2s_set_pin(I2S_NUM_0, &pins);
}

// ============================================================================
//  MISE À L'ÉCHELLE — 32-bit I2S → 16-bit PCM, avec AGC + noise gate
// ============================================================================
//  s_buff : buffer brut I2S (32-bit, len octets)
//  d_buff : buffer destination PCM 16-bit (len/2 octets)
//  Retourne le peak brut mesuré (utile pour diagnostic/LED)
// ============================================================================
static float i2s_adc_data_scale(uint8_t *d_buff, const uint8_t *s_buff, uint32_t len)
{
    static float gain     = 1.0f;
    const float GAIN_MAX  = 3.0f;
    const float GAIN_MIN  = 1.0f;
    const float TARGET    = 8000.0f;
    const float ATTACK    = 0.01f;
    const float RELEASE   = 0.001f;

    // ── Noise gate ────────────────────────────────────────
    const float GATE_OPEN  = 600.0f;  // seuil pour ouvrir (son détecté)
    const float GATE_CLOSE = 300.0f;  // seuil pour fermer (silence)
    static bool gate_open    = false;
    static int  hold_samples = 0;
    const int   HOLD_MAX     = 30;    // blocs gardés ouverts après le son

    // ── 1. Mesurer le pic brut (avant gain) ──────────────
    float peak = 0;
    for (uint32_t i = 0; i < len; i += 4) {
        int32_t raw32 = (int32_t)(
            ((uint32_t)s_buff[i + 3] << 24) |
            ((uint32_t)s_buff[i + 2] << 16) |
            ((uint32_t)s_buff[i + 1] << 8)  |
            ((uint32_t)s_buff[i + 0]));
        float s = fabsf((float)(int16_t)(raw32 >> 14));
        if (s > peak) peak = s;
    }

    // ── 2. Logique gate (avec hold pour éviter la coupure) ──
    if (peak > GATE_OPEN) {
        gate_open    = true;
        hold_samples = HOLD_MAX;
    } else if (peak < GATE_CLOSE) {
        if (hold_samples > 0) hold_samples--;
        else gate_open = false;
    }

    // ── 3. AGC — seulement si gate ouvert ────────────────
    if (gate_open && peak > 0.1f) {
        float desired = TARGET / peak;
        if (desired < gain) gain += (desired - gain) * ATTACK;
        else                 gain += (desired - gain) * RELEASE;
        if (gain > GAIN_MAX) gain = GAIN_MAX;
        if (gain < GAIN_MIN) gain = GAIN_MIN;
    }

    // ── 4. Appliquer gain + gate ──────────────────────────
    uint32_t j = 0;
    for (uint32_t i = 0; i < len; i += 4) {
        int32_t raw32 = (int32_t)(
            ((uint32_t)s_buff[i + 3] << 24) |
            ((uint32_t)s_buff[i + 2] << 16) |
            ((uint32_t)s_buff[i + 1] << 8)  |
            ((uint32_t)s_buff[i + 0]));
        int16_t sample = (int16_t)(raw32 >> 14);
        

        int32_t amplified = 0; // silence si gate fermé
        if (gate_open) {
            amplified = (int32_t)((float)sample * gain);
            if (amplified > 32767)  amplified = 32767;
            if (amplified < -32768) amplified = -32768;
        }

        d_buff[j++] = (uint8_t)(amplified & 0xFF);
        d_buff[j++] = (uint8_t)((amplified >> 8) & 0xFF);
    }

    return peak;
}

// ============================================================================
//  HEADER WAV — 16-bit PCM (AUDIO_WAV_BITS), basé sur AUDIO_SAMPLE_RATE
// ============================================================================
static void wavHeader(byte *h, int wavSize)
{
    h[0] = 'R'; h[1] = 'I'; h[2] = 'F'; h[3] = 'F';
    unsigned int fs = wavSize + AUDIO_WAV_HEADER_SIZE - 8;
    h[4] = (byte)fs; h[5] = (byte)(fs >> 8);
    h[6] = (byte)(fs >> 16); h[7] = (byte)(fs >> 24);

    h[8] = 'W'; h[9] = 'A'; h[10] = 'V'; h[11] = 'E';
    h[12] = 'f'; h[13] = 'm'; h[14] = 't'; h[15] = ' ';
    h[16] = 0x10; h[17] = 0x00; h[18] = 0x00; h[19] = 0x00;
    h[20] = 0x01; h[21] = 0x00;
    h[22] = AUDIO_CHANNELS; h[23] = 0x00;

    uint32_t sr = AUDIO_SAMPLE_RATE;
    h[24] = (byte)sr; h[25] = (byte)(sr >> 8);
    h[26] = (byte)(sr >> 16); h[27] = (byte)(sr >> 24);

    uint32_t br = sr * AUDIO_CHANNELS * (AUDIO_WAV_BITS / 8);
    h[28] = (byte)br; h[29] = (byte)(br >> 8);
    h[30] = (byte)(br >> 16); h[31] = (byte)(br >> 24);

    uint16_t ba = AUDIO_CHANNELS * (AUDIO_WAV_BITS / 8);
    h[32] = (byte)ba; h[33] = (byte)(ba >> 8);

    h[34] = AUDIO_WAV_BITS; h[35] = 0x00;

    h[36] = 'd'; h[37] = 'a'; h[38] = 't'; h[39] = 'a';
    h[40] = (byte)wavSize; h[41] = (byte)(wavSize >> 8);
    h[42] = (byte)(wavSize >> 16); h[43] = (byte)(wavSize >> 24);
}

// ============================================================================
//  COMPTEUR SÉQUENTIEL PERSISTANT
// ============================================================================
static uint32_t loadSeq()
{
    if (!SD.exists(AUDIO_SEQ_FILE)) return 0;
    File f = SD.open(AUDIO_SEQ_FILE, FILE_READ);
    if (!f) return 0;
    uint32_t n = f.parseInt();
    f.close();
    return n;
}

static void saveSeq(uint32_t n)
{
    File f = SD.open(AUDIO_SEQ_FILE, FILE_WRITE);
    if (!f) return;
    f.print(n);
    f.close();
}

// ============================================================================
//  TÂCHE D'ENREGISTREMENT (non-bloquante pour la loop principale)
// ============================================================================
static void recordTask(void *arg)
{
    recording = true;
    stopRequested = false;

    // 1) Préparer le chemin du fichier
    String networkTime = getNetworkTime();
    char timeBuf[32] = "";

    if (networkTime.length() >= 19)
    {
        // Évite le cas où le modem retourne "80/01/06,00:00:00" (default avant sync)
        int year = networkTime.substring(0, 4).toInt();
        if (year >= 2024 && year < 2100)
        {
            snprintf(timeBuf, sizeof(timeBuf),
                     "%c%c%c%c-%c%c-%c%c_%c%c-%c%c-%c%c",
                     networkTime[0], networkTime[1], networkTime[2], networkTime[3],
                     networkTime[5], networkTime[6],
                     networkTime[8], networkTime[9],
                     networkTime[11], networkTime[12],
                     networkTime[14], networkTime[15],
                     networkTime[17], networkTime[18]);
        }
        else
        {
            Serial.printf("[AUDIO] Network time year invalid (%d), using fallback\n", year);
        }
    }

     // Vérifier que le timeBuf ne contient pas de caractères invalides FAT
    bool timeBufValid = (timeBuf[0] != 0);
    if (timeBufValid)
    {
        for (int k = 0; timeBuf[k] != 0; k++)
        {
            char c = timeBuf[k];
            // Caractères autorisés : chiffres, tiret, underscore
            if (!isdigit(c) && c != '-' && c != '_')
            {
                timeBufValid = false;
                Serial.printf("[AUDIO] Invalid char '%c' in timestamp, using uptime fallback\n", c);
                break;
            }
        }
    }

    if (timeBufValid)
    {
        snprintf(lastFilePath, sizeof(lastFilePath),
                 "%s/alert_%s.wav", AUDIO_DIR_SD, timeBuf);
    }
    else
    {
        uint32_t uptimeSec = millis() / 1000;
        snprintf(lastFilePath, sizeof(lastFilePath),
                 "%s/alert_uptime_%u.wav", AUDIO_DIR_SD, uptimeSec);
        Serial.println("[AUDIO] Using uptime fallback for filename");
    }
    //----------------------------------------------------

    lastSeq++;
    saveSeq(lastSeq);
    Serial.printf("[AUDIO] Recording to %s\n", lastFilePath);

    // 2) Ouvrir le fichier
    File file = SD.open(lastFilePath, FILE_WRITE);
    if (!file)
    {
        Serial.println("[AUDIO] ERROR: cannot open file on SD");
        recording = false;
        recordTaskHandle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // 3) Écrire le header WAV (provisoire avec la taille théorique)
    byte header[AUDIO_WAV_HEADER_SIZE];
    wavHeader(header, AUDIO_FILE_SIZE);
    file.write(header, AUDIO_WAV_HEADER_SIZE);

    // 4) Allouer les buffers — lecture 32-bit (read), écriture 16-bit (write = moitié)
    uint8_t *read_buff  = (uint8_t *)calloc(AUDIO_I2S_READ_LEN, 1);
    uint8_t *write_buff = (uint8_t *)calloc(AUDIO_I2S_READ_LEN / 2, 1);
    if (!read_buff || !write_buff)
    {
        Serial.println("[AUDIO] ERROR: out of memory");
        file.close();
        free(read_buff);
        free(write_buff);
        recording = false;
        recordTaskHandle = NULL;
        vTaskDelete(NULL);
        return;
    }
     // Prendre le mutex I2S avant les lectures de chauffe
    if (!i2sResource_take()) {
        Serial.println("[AUDIO] Cannot take I2S mutex, aborting recording");
        file.close();
        free(read_buff);
        free(write_buff);
        recording = false;
        recordTaskHandle = NULL;
        vTaskDelete(NULL);
        return;
    }
    Serial.println("[AUDIO] I2S mutex taken (recording)");

    // 5) Lectures de chauffe (2 reads pour stabiliser le DMA + l'AGC)
    size_t bytes_read;
    i2s_read(I2S_NUM_0, read_buff, AUDIO_I2S_READ_LEN, &bytes_read, portMAX_DELAY);
    i2s_read(I2S_NUM_0, read_buff, AUDIO_I2S_READ_LEN, &bytes_read, portMAX_DELAY);

    // Diagnostic peak (32-bit correct)
    float warmupPeak = 0;
    for (uint32_t k = 0; k < bytes_read; k += 4) {
        int32_t raw = (int32_t)(
            ((uint32_t)read_buff[k + 3] << 24) |
            ((uint32_t)read_buff[k + 2] << 16) |
            ((uint32_t)read_buff[k + 1] << 8)  |
            ((uint32_t)read_buff[k + 0]));
        float s = fabsf((float)(int16_t)(raw >> 8));
        if (s > warmupPeak) warmupPeak = s;
    }
    Serial.printf("[AUDIO] PEAK after warmup: %.0f / 32767\n", warmupPeak);

    // 6) Boucle d'enregistrement (stop sur taille atteinte OU stopRequested)
    int total_written = 0;
    int lastPct = -1;
    while (total_written < AUDIO_FILE_SIZE && !stopRequested)
    {
        i2s_read(I2S_NUM_0, read_buff, AUDIO_I2S_READ_LEN, &bytes_read, portMAX_DELAY);

        float peak = i2s_adc_data_scale(write_buff, read_buff, bytes_read);

        // bytes_read est en 32-bit (4 octets/sample) → sortie 16-bit = moitié
        size_t pcm_len = bytes_read / 2;
        size_t remaining = (size_t)(AUDIO_FILE_SIZE - total_written);
        if (pcm_len > remaining) pcm_len = remaining;

        file.write(write_buff, pcm_len);
        total_written += (int)pcm_len;

        int pct = total_written * 100 / AUDIO_FILE_SIZE;
        if (pct / 10 != lastPct / 10)
        {
            Serial.printf("[AUDIO] %d%% (peak=%.0f)\n", pct, peak);
            lastPct = pct;
        }
    }

    // 7) Si stop anticipé, mettre à jour le header avec la VRAIE taille
    if (stopRequested && total_written < AUDIO_FILE_SIZE)
    {
        file.seek(0);
        byte realHeader[AUDIO_WAV_HEADER_SIZE];
        wavHeader(realHeader, total_written);
        file.write(realHeader, AUDIO_WAV_HEADER_SIZE);
        Serial.printf("[AUDIO] Stopped early, header updated (%d bytes audio)\n",
                      total_written);
    }


    // 8) Fermeture
    file.flush();
    file.close();
      // Forcer le flush de la FAT directory
    SD.end();
    delay(100);
    extern SPIClass sdSpi;
    sdSpi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    SD.begin(PIN_SD_CS, sdSpi);
    Serial.println("[AUDIO] SD remounted after recording");

    // Vérification
    if (SD.exists(lastFilePath)) {
        Serial.printf("[AUDIO] File confirmed on SD: %s\n", lastFilePath);
    } else {
        Serial.println("[AUDIO] ERROR: file not found after remount!");
    }
    //---------------------------------------
    free(read_buff);
    free(write_buff);
    // Libérer le mutex I2S AVANT de quitter
    i2sResource_give();
    Serial.println("[AUDIO] I2S mutex released (recording)");

    Serial.printf("[AUDIO] Recording done: %s (%d bytes audio)\n",
                  lastFilePath, total_written);

    recording = false;
    stopRequested = false;
    recordTaskHandle = NULL;
    vTaskDelete(NULL);
}

// ============================================================================
//  LECTURE I2S NON-BLOQUANTE — pour le live stream 
// ============================================================================


size_t audio_streamReadChunk(int16_t *outPcm, size_t maxSamples)
{
    if (recording) return 0;              // Option A : jamais pendant l'enregistrement SOS
    if (maxSamples == 0) return 0;
    if (!i2sResource_take()) return 0;    // I2S occupé (voice_trigger) → on renonce ce tick

    static uint8_t raw32[AUDIO_I2S_READ_LEN];
    size_t wantBytes = maxSamples * 4;                       // 4 octets/sample (32-bit)
    if (wantBytes > AUDIO_I2S_READ_LEN) wantBytes = AUDIO_I2S_READ_LEN;

    size_t bytesRead = 0;
    esp_err_t err = i2s_read(I2S_NUM_0, raw32, wantBytes, &bytesRead, 0); // timeout 0 = non bloquant
    if (err != ESP_OK || bytesRead == 0)
    {
        i2sResource_give();
        return 0;
    }

    // 32-bit (bytesRead octets) → 16-bit PCM (bytesRead/2 octets = bytesRead/4 samples)
    i2s_adc_data_scale((uint8_t *)outPcm, raw32, bytesRead);
    i2sResource_give();

    return bytesRead / 4;                 // nombre de samples int16 écrits
}

// ============================================================================
//  API PUBLIQUE
// ============================================================================
bool audio_init()
{
    
    Serial.println("[AUDIO] Init...");

    // Test SD direct
    Serial.printf("[AUDIO] SD cardSize: %llu\n", SD.cardSize());
    Serial.printf("[AUDIO] SD cardType: %d\n", SD.cardType());
    Serial.printf("[AUDIO] /alerts exists: %s\n", 
                  SD.exists("/alerts") ? "YES" : "NO");
    Serial.printf("[AUDIO] /audio_seq.txt exists: %s\n",
                  SD.exists(AUDIO_SEQ_FILE) ? "YES" : "NO");

    if (!SD.cardSize())
    {
        Serial.println("[AUDIO] ERROR: SD not initialized");
        return false;
    }
    Serial.printf("[AUDIO] Using existing SD, size=%lluMB\n",
                  SD.cardSize() / (1024 * 1024));

    if (!SD.exists(AUDIO_DIR_SD))
    {
        SD.mkdir(AUDIO_DIR_SD);
    }

    lastSeq = loadSeq();
    Serial.printf("[AUDIO] Last seq number: %u\n", lastSeq);

    i2s_init();
    Serial.printf("[AUDIO] I2S initialized 32-bit @ %u Hz (WS=%d, SCK=%d, SD=%d)\n",
                  AUDIO_SAMPLE_RATE, PIN_I2S_WS, PIN_I2S_SCK, PIN_I2S_SD);

    return true;
}

bool audio_startRecording()
{
    if (recording)
    {
        Serial.println("[AUDIO] Already recording");
        return false;
    }

    BaseType_t ok = xTaskCreate(
        recordTask, "audio_rec", 8 * 1024, NULL, 1, &recordTaskHandle);

    if (ok != pdPASS)
    {
        Serial.println("[AUDIO] ERROR: cannot create record task");
        return false;
    }
    return true;
}

void audio_stopRecording()
{
    if (!recording) return;
    Serial.println("[AUDIO] Stop requested");
    stopRequested = true;
}

bool audio_isRecording()
{
    return recording;
}

uint32_t audio_getLastSeq()

{
    return lastSeq;
}

const char *audio_getLastFilePath()
{
    return lastFilePath;
}