#ifndef OTA_LTE_H
#define OTA_LTE_H

#include <Arduino.h>

// ============================================================================
//  OTA via LTE — téléchargement HTTP par commandes AT (SIM7670G)
// ============================================================================
//  Séquence AT utilisée :
//    AT+HTTPINIT          → initialise le service HTTP du modem
//    AT+HTTPPARA          → configure l'URL et les headers
//    AT+HTTPGET           → déclenche le téléchargement dans le buffer interne
//    AT+HTTPREAD=0,<n>    → lit le binaire chunk par chunk vers l'ESP32
//    AT+HTTPTERM          → libère le service HTTP
//
//  La vérification SHA256 et l'écriture flash (Update.*) sont faites côté
//  ESP32 exactement comme dans le chemin WiFi — même logique, transport seul
//  change.
//
//  Taille max d'un chunk HTTPREAD : 1460 octets (limite SIM7670G).
//  La fonction gère la boucle de lecture jusqu'à contentLength octets.
// ============================================================================

// Télécharge le firmware via LTE et l'écrit dans la partition OTA.
// url          : URL HTTP complète (ex: "http://172.29.26.209:8080/api/firmwares/2/download")
// expectedSha256 : 64 hex chars (lowercase)
// expectedSize   : taille du .bin en octets
// Retourne false en cas d'échec. En cas de succès → ESP.restart() (ne retourne pas).
bool performOTA_LTE(const String& url,
                    const String& expectedSha256,
                    size_t        expectedSize);

// Nettoyage du service HTTP du modem (HTTPTERM).
// Appelé automatiquement en fin de performOTA_LTE, mais exposé pour les cas
// d'erreur où on veut forcer le cleanup depuis l'extérieur.
void otaLte_cleanup();

#endif