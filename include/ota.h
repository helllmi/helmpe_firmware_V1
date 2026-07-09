#ifndef OTA_H
#define OTA_H

#include <Arduino.h>

// ============================================================================
//  OTA - téléchargement HTTP + vérif SHA256 + écriture flash
// ============================================================================
// Pour l'instant : téléchargement HTTP direct via Wi-Fi (HTTPClient).
// Plus tard : variante via AT+HTTP du SIM7670G pour OTA en LTE Cat M1.

// Lance le processus complet : download → SHA256 → Update.write → reboot
// expectedSha256 : 64 hex chars (lowercase)
// expectedSize   : taille du .bin en octets (utilisée pour init Update)
// Retourne false en cas d'échec. En cas de succès → ESP.restart() (ne retourne pas)
bool performOTA_WIFI(const String& url,
                const String& expectedSha256,
                size_t expectedSize);
// Point d'entrée unique — route automatiquement vers WiFi ou LTE
bool performOTA(const String& url,
                const String& expectedSha256,
                size_t        expectedSize);
// Drapeau anti-réentrance (lu par le callback MQTT)
extern bool otaInProgress;

// Debug : affiche l'état des partitions OTA
void debugAllPartitions();

// Conversion uint8_t[32] → string hex
void shaToHex(const uint8_t* hash, String& out);

#endif
