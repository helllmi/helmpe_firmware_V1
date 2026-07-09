#ifndef SOS_ALERT_H
#define SOS_ALERT_H

#include <Arduino.h>

// ============================================================================
//  SOS_ALERT — orchestrateur du flux d'alerte HEELPMEE
// ============================================================================
//  Appelé à l'entrée de STATE_ACTION (state_machine.cpp). Fait :
//   1) Générer un record_id (UUID v4)
//   2) Récupérer position (GPS si fix valide, sinon défaut config.h)
//   3) Publier l'alerte MQTT (helpme/{id}/sos) via mqttTransport_publishSos
//   4) Mémoriser le record_id courant pour le lier à l'upload audio
//      qui sera déclenché séparément une fois l'enregistrement terminé.
// ============================================================================

// Déclenche le flux SOS (idempotent — ignoré si déjà en cours)
void sosAlert_trigger();

// Record ID de l'alerte SOS en cours (vide si aucune alerte active)
const String& sosAlert_getCurrentRecordId();

// Réinitialise l'état (appelé après upload terminé ou retour STANDBY)
void sosAlert_reset();

#endif