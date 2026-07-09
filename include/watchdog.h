#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <Arduino.h>

// ============================================================================
//  CONFIGURATION
// ============================================================================
#define WDT_TIMEOUT_S 60   // watchdog hardware : 60s

// Fenêtre d'observation post-OTA
#define OTA_OBSERVATION_PERIOD_MS    (60 * 1000)   // 1 min (tests) - 5min en prod
#define OTA_HEALTH_CHECK_INTERVAL_MS (5  * 1000)   // check toutes les 5s
#define OTA_MAX_FAILURES             3             // rollback après 3 échecs

// ============================================================================
//  WATCHDOG HARDWARE (Task Watchdog Timer)
// ============================================================================
void setupWatchdog();
void feedWatchdog();

// ============================================================================
//  DRAPEAU NVS "OTA pending validation"
// ============================================================================
void markOtaPendingValidation();
bool isOtaPendingValidation();
void confirmOtaSuccess();

// ============================================================================
//  OBSERVATION POST-OTA
// ============================================================================
// À appeler dans setup() : démarre la fenêtre si le drapeau NVS est posé
void runSelfTestAfterOTA();

// À appeler dans loop() : exécute les checks périodiques, déclenche rollback si KO
void otaObservationTick();

// Indique si on est actuellement dans la fenêtre d'observation
bool isOtaObserving();

// ============================================================================
//  ROLLBACK MANUEL
// ============================================================================
void manualRollback();

#endif
