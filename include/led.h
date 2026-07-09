#ifndef LED_H
#define LED_H
#include <Adafruit_NeoPixel.h>

// ============================================================================
//  LED NeoPixel - PIN & SETUP
// ============================================================================
#define LED_PIN    38
#define LED_COUNT  1

extern Adafruit_NeoPixel strip;

// ============================================================================
//  COULEURS - États existants (gardés tels quels)
// ============================================================================
#define LED_OFF          strip.Color(0,   0,   0)
#define LED_STARTUP      strip.Color(255, 255, 0)    // jaune   → démarrage
#define LED_GPS_SEARCH   strip.Color(0,   0,   255)  // bleu    → cherche GPS
#define LED_GPS_OK       strip.Color(0,   255, 0)    // vert    → GPS fixé
#define LED_LTE_SENDING  strip.Color(255, 165, 0)    // orange  → envoi LTE (futur)
#define LED_WIFI_SENDING strip.Color(0,   255, 255)  // cyan    → publish MQTT en Wi-Fi
#define LED_SUCCESS      strip.Color(255, 255, 255)  // blanc   → publish OK
#define LED_ERROR        strip.Color(255, 0,   0)    // rouge   → erreur
#define LED_LTE_SETUP    strip.Color(255, 0,   255)  // violet  → setup LTE

// ============================================================================
//  COULEURS - Nouveaux états (OTA + watchdog)
// ============================================================================
#define LED_MQTT_CONNECTING strip.Color(50,  50,  200)  // bleu sombre → connexion MQTT
#define LED_MQTT_OK         strip.Color(0,   100, 50)   // vert sombre → MQTT connecté
#define LED_OTA_DOWNLOAD    strip.Color(180, 0,   180)  // violet      → download firmware
#define LED_OTA_INSTALL     strip.Color(255, 0,   200)  // magenta     → écriture flash
#define LED_OTA_OBSERVE     strip.Color(0,   50,  200)  // bleu pulsé  → observation post-OTA
#define LED_ROLLBACK        strip.Color(200, 0,   50)   // rouge pulsé → rollback

// ============================================================================
//  FONCTIONS
// ============================================================================
void ledBegin();
void setLED(uint32_t color);
void blinkLED(uint32_t color, int times = 3, int delayMs = 200);

// Heartbeat non-bloquant à appeler depuis loop()
// pattern = couleur à utiliser, intervalMs = période d'allumage
void ledHeartbeatTick(uint32_t color, uint32_t intervalMs);

#endif
