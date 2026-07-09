/**
 * ===========================================================================
 *  sos_button.h — Détection du triple clic SOS
 * ===========================================================================
 *  Module non-bloquant qui détecte un triple clic rapide sur le bouton SOS.
 *  À chaque appel de sosButton_tick(), il met à jour son état interne.
 *  Quand un triple clic est détecté, sosButton_wasTripleClicked() renvoie
 *  true une seule fois (puis se reset automatiquement).
 *
 *  Capture aussi le press_count et le press_duration_ms du dernier appui,
 *  pour remplir le JSON d'alerte (champs requis par le backend).
 * ===========================================================================
 */
#ifndef SOS_BUTTON_H
#define SOS_BUTTON_H

#include <Arduino.h>

// ============================================================================
//  API PUBLIQUE
// ============================================================================

// À appeler une fois dans setup()
void sosButton_init();

// À appeler très régulièrement dans loop() (toutes les ~10ms idéalement)
void sosButton_tick();

// Renvoie true UNE SEULE FOIS quand un triple clic vient d'être détecté.
// Après l'appel, le drapeau est consommé (revient à false).
bool sosButton_wasTripleClicked();

// État instantané du bouton (utile pour debug)
bool sosButton_isPressed();


bool sosButton_wasLongPressed();
// Durée actuelle d'appui en cours (utile pour feedback LED en live)
uint32_t sosButton_getCurrentPressDurationMs();

// ============================================================================
//  DONNÉES DU DERNIER TRIPLE CLIC (pour le JSON d'alerte)
// ============================================================================

// Nombre de clics du dernier triple-clic détecté (toujours 3)
uint8_t sosButton_getPressCount();

// Durée totale du triple-clic en ms (du premier appui au 3e relâchement)
uint32_t sosButton_getPressDurationMs();

#endif