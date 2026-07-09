#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ============================================================================
//  IDENTIFICATION DU DEVICE
// ============================================================================
#define DEVICE_ID "PL601-00002"

// ============================================================================
//  MQTT
// ============================================================================
#define MQTT_BROKER_HOST "helpmee.nacloud.tn" // IP de ton PC qui héberge Mosquitto
#define MQTT_BROKER_PORT 1883
#define MQTT_KEEPALIVE_SEC 30
#define MQTT_BUFFER_SIZE 2048 // taille du buffer interne PubSubClient

// ============================================================================
//  LTE / APN
// ============================================================================
#define LTE_APN "internet.ooredoo.tn"


// ============================================================================
//  TIMINGS DU LOOP PRINCIPAL
// ============================================================================
#define ALERT_INTERVAL_MS 100000              // alerte toutes les 10s minimum
#define TELEMETRY_INTERVAL_STANDBY_MS 300000 // 10 min en STANDBY

#define STATE_HEARTBEAT_STANDBY_MS 300000 // 10 min en STANDBY


#define TELEMETRY_INTERVAL_MS TELEMETRY_INTERVAL_STANDBY_MS

#define WIFI_RETRY_INTERVAL_MS 10000  // retry connexion WiFi si down
#define MQTT_RETRY_INTERVAL_MS 5000   // retry connexion MQTT si down
#define WIFI_CONNECT_TIMEOUT_MS 15000 // timeout connexion WiFi initiale
#define DELETE_LOG_INTERVAL_MS (5 * 60000)
#define MAIN_LOOP_DELAY_MS 2000 // delay() à la fin de loop()

// ============================================================================
//  PINS GPIO — LilyGo T-SIM7670G-S3
// ============================================================================
// SIM7670G (UART1)
#define PIN_SIM_RX 10   // MODEM_RX (ESP reçoit du modem)
#define PIN_SIM_TX 11   // MODEM_TX (ESP envoie au modem)

// Contrôle modem LilyGo (obligatoire)
#define MODEM_PWRKEY_PIN 18
#define MODEM_RESET_PIN  17
#define MODEM_DTR_PIN    9
#define MODEM_RESET_LEVEL LOW
#define MODEM_POWERON_PULSE_WIDTH_MS 1000
#define MODEM_RESET_PULSE_WIDTH_MS   2600

// I2C
#define PIN_I2C_SDA 2
#define PIN_I2C_SCL 1

// Alimentation périphériques externes (pas de pin dédié sur LilyGo)
#define PIN_POWER_PERIPH -1

// Carte SD — mode SPI (SD_MMC non câblé sur LilyGo)
#define PIN_SD_SCK  21
#define PIN_SD_MOSI 14
#define PIN_SD_MISO 47
#define PIN_SD_CS   13

// Bouton SOS
#define PIN_SOS_BUTTON 15
#define SOS_TRIPLE_WINDOW_MS 800 // fenêtre totale pour les 3 clics
#define SOS_DEBOUNCE_MS 50       // anti-rebond logiciel
#define SOS_CLICK_MAX_MS 300 
#define LONG_PRESS_MS 3000   // durée min d'un appui long
// durée max d'un clic court (sinon = long press)
// ============================================================================
//  STORAGE - Buffer offline LittleFS
// ============================================================================
#define STORAGE_MAX_MESSAGES 100
#define STORAGE_DIR_QUEUE "/queue"

// ============================================================================
//  POWER MANAGEMENT
// ============================================================================
#define CPU_FREQ_ACTION_MHZ 240 // pleine puissance en ACTION
#define CPU_FREQ_STANDBY_MHZ 80 // économie en STANDBY
#define CPU_FREQ_BOOT_MHZ 240   // pleine puissance au boot
// NeoPixel : défini dans led.h (PIN 38)

// ============================================================================
//  TRANSPORT MQTT — choix WiFi / LTE              ← NOUVEAU BLOC
// ============================================================================
#define MQTT_TRANSPORT_WIFI 0
#define MQTT_TRANSPORT_LTE 1

// Transport actif. Pour l'instant WiFi (testé). Passer à _LTE quand le
// device est dispo pour tester le MQTT 4G.
#define MQTT_TRANSPORT MQTT_TRANSPORT_LTE

// ============================================================================
//  AUDIO - Microphone INMP441 via I2S
// ============================================================================
// Pins I2S (à adapter si conflit sur ta carte)
#define PIN_I2S_WS 6   // Word Select (LRCK)
#define PIN_I2S_SD 7   // Serial Data (DOUT du micro)
#define PIN_I2S_SCK 8  // Serial Clock (BCLK)

// Paramètres audio
#define AUDIO_SAMPLE_RATE 16000 // 16 kHz (voix, lecture native I2S 32-bit)
#define AUDIO_SAMPLE_BITS 32    // lecture I2S brute (micro INMP441 natif)
#define AUDIO_WAV_BITS 16       // écriture WAV en 16 bits (après scale 32→16)
#define AUDIO_CHANNELS 1        // mono
#define AUDIO_DURATION_SEC 3 // 5s par alerte — chunké en 4x4s pour respecter AT+HTTPDATA (LTE, max 153600 octets/requête)
#define AUDIO_I2S_READ_LEN 4096 // taille buffer lecture I2S (32-bit = 1024 samples)
#define AUDIO_WAV_HEADER_SIZE 44

// Taille totale d'un fichier audio WAV (en octets, données PCM 16-bit, sans header)
#define AUDIO_FILE_SIZE (AUDIO_CHANNELS * AUDIO_SAMPLE_RATE * (AUDIO_WAV_BITS / 8) * AUDIO_DURATION_SEC)

// Stockage sur SD
#define AUDIO_DIR_SD "/alerts"          // dossier sur la carte SD
#define AUDIO_SEQ_FILE "/audio_seq.txt" // fichier compteur (sur SD)

// ============================================================================
//  PORTAIL CAPTIF (config locale via AP WiFi)
// ============================================================================
#define CAPTIVE_AP_SSID         "HEELPMEE-Config"   // SSID de l'AP local
#define CAPTIVE_AP_PASSWORD     ""                   // "" = AP ouvert
#define CAPTIVE_AP_CHANNEL      6                    // canal WiFi
#define CAPTIVE_AP_MAX_CLIENTS  2                    // max 2 téléphones connectés
#define CAPTIVE_TIMEOUT_MS      120000               // auto-shutdown 2 min

// ============================================================================
//  SMS HANDLER — commandes à distance via SIM7670G
// ============================================================================
// Numéro autorisé à envoyer des commandes SMS (format international)
#define SMS_AUTHORIZED_NUMBER "+21655432789"

// Intervalle de polling SMS dans loop() en ms
#define SMS_POLL_INTERVAL_MS 5000

// ============================================================================
//  PLATEFORME CLOUD HEELPMEE — SOS MQTT + upload audio HTTP
// ============================================================================
// Topics MQTT (format helpme/%s/... avec %s = DEVICE_ID, snprintf)
#define MQTT_TOPIC_SOS_FMT   "helpme/%s/sos"
#define MQTT_TOPIC_TOKEN_FMT "helpme/%s/token"

// Serveur d'upload audio (HTTP multipart)
#define CLOUD_SERVER_HOST   "helpmee.nacloud.tn"
#define CLOUD_SERVER_PORT   3000
#define CLOUD_UPLOAD_PATH   "/audio/upload"

// JWT fallback si NVS vide (le vrai token sera reçu via MQTT_TOPIC_TOKEN)
#define JWT_TOKEN_FALLBACK  "tCV7X_lALEo9wDyQw8prMg"

// NVS namespace/key pour le token JWT persistant
#define NVS_NAMESPACE_JWT   "heelpmee"
#define NVS_KEY_JWT_TOKEN   "jwt_token"

// Géoloc par défaut si pas de fix GPS au moment du SOS (Tunis)
#define SOS_DEFAULT_LATITUDE  36.8065
#define SOS_DEFAULT_LONGITUDE 10.1815

// Upload / retry
#define UPLOAD_MAX_RETRIES       3
#define UPLOAD_RETRY_DELAY_MS    2000
#define UPLOAD_RESPONSE_TIMEOUT_MS 15000

// Chunking upload LTE (AT+HTTPDATA max 153600 octets/requête)
// 4s à 16kHz/16-bit/mono = 128000 octets PCM, sous la limite avec marge multipart
//#define UPLOAD_LTE_CHUNK_AUDIO_BYTES 128000
#define UPLOAD_LTE_CHUNK_AUDIO_BYTES 120000
#define UPLOAD_CHUNK_MAX_RETRIES      3
//#define LTE_BAUD_230400
#endif