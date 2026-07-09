#ifndef JWT_STORAGE_H
#define JWT_STORAGE_H

#include <Arduino.h>

// ============================================================================
//  JWT_STORAGE — persistance du token d'authentification upload (NVS)
// ============================================================================
//  Le token est reçu via MQTT (topic helpme/{id}/token) et sauvegardé en NVS.
//  Au boot, on charge depuis NVS ; si vide, on utilise JWT_TOKEN_FALLBACK.
// ============================================================================



// Sauvegarde un nouveau token en NVS + met à jour la valeur en mémoire.
// Vérifie l'écriture par relecture.
bool jwtStorage_save(const String& token);
void jwtStorage_load();
const String& jwtStorage_get();

#endif