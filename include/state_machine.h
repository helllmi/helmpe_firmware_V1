/**
 * ===========================================================================
 *  state_machine.h — Machine d'États du Firmware HEELPMEE
 * ===========================================================================
 *  3 états : OFF / STANDBY / ACTION
 *
 *  Transitions :
 *    OFF      → STANDBY  : tous les modules initialisés (EVT_BOOT_OK)
 *    STANDBY  → ACTION   : triple clic SOS détecté (EVT_SOS_TRIGGERED)
 *    ACTION   → STANDBY  : re-triple clic (reset utilisateur, EVT_USER_RESET)
 *
 *  Toute action métier (envoi alerte, fréquence GPS, etc.) doit consulter
 *  l'état courant via stateMachine_getState() avant de s'exécuter.
 * ===========================================================================
 */
#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

#include <Arduino.h>

// ============================================================================
//  ÉTATS
// ============================================================================
enum DeviceState {
    STATE_OFF     = 0,    // boot, init en cours
    STATE_STANDBY = 1,    // prêt, en veille active
    STATE_ACTION  = 2     // alerte SOS active
};

// ============================================================================
//  ÉVÉNEMENTS (entrées de la FSM)
// ============================================================================
enum DeviceEvent {
    EVT_BOOT_OK       = 100,   // tous les modules sont initialisés
    EVT_SOS_TRIGGERED = 101,   // long clic clic en STANDBY
    EVT_USER_RESET    = 102    // long clic en ACTION (annulation)
};

// ============================================================================
//  API PUBLIQUE
// ============================================================================

// Init : à appeler une fois dans setup()
void stateMachine_init();

// Dispatcher un événement à la FSM
// La FSM décide elle-même si l'événement est valide dans l'état courant
void stateMachine_dispatch(DeviceEvent evt);

// Récupérer l'état courant
DeviceState stateMachine_getState();

// Nom lisible de l'état (utile pour logs)
const char* stateMachine_stateName(DeviceState s);

// Depuis combien de temps on est dans l'état courant
uint32_t stateMachine_getStateUptime();
typedef void (*StateChangeCallback)(DeviceState newState);
void stateMachine_onStateChange(StateChangeCallback cb);

// À appeler à chaque tour de loop() — surveille la fin de l'enregistrement
// audio pendant STATE_ACTION et déclenche l'upload (tâche FreeRTOS séparée).
void stateMachine_tick();
#endif