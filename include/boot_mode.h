/**
 * boot_mode.h — Gestion du flag de transport à utiliser au prochain boot
 *
 * Permet de stocker en NVS un override de transport :
 *   - normal : on suit la stratégie par défaut (LTE prioritaire)
 *   - forceWifi : au prochain boot, on saute LTE et on va direct WiFi
 *
 * Utilisé pour le fallback LTE → WiFi : quand le LTE échoue trop,
 * on set forceWifi=true puis on reboot.
 */
#ifndef BOOT_MODE_H
#define BOOT_MODE_H

#include <Arduino.h>

void bootMode_init();

// Lire le flag forceWifi (true = au boot, utiliser WiFi directement)
bool bootMode_isForceWifi();

// Set le flag pour le prochain boot
void bootMode_setForceWifi(bool force);

// Reset à la valeur par défaut
void bootMode_reset();

#endif