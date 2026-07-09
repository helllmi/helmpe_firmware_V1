#ifndef AUDIO_UPLOAD_H
#define AUDIO_UPLOAD_H

#include <Arduino.h>


// ============================================================================
//  AUDIO_UPLOAD — envoi du WAV au serveur HEELPMEE (HTTP multipart)
// ============================================================================
//  Route automatiquement selon le transport actif (mqttTransport_isWifi()) :
//   - WiFi : socket TCP brut + multipart manuel (identique au code de
//            référence de l'encadrant, juste lu depuis SD_MMC)
//   - LTE  : AT+HTTPPARA + AT+HTTPDATA (upload du body vers le modem) +
//            AT+HTTPACTION=1 (POST)
//
//  Headers envoyés (les deux chemins) :
//    Authorization: Bearer <jwt depuis jwtStorage_get()>
//    X-Device-Id: DEVICE_ID
//    X-Record-Id: recordId
//    Content-Type: multipart/form-data; boundary=...
// ============================================================================

// Upload le fichier WAV à filePath, associé au record_id de l'alerte SOS.
// Retourne true si HTTP 2xx reçu. Ne bloque jamais indéfiniment (timeouts).
bool audioUpload_send(const String& filePath, const String& recordId);

#endif