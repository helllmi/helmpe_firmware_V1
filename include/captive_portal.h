/**
 * captive_portal.h — Portail de configuration locale
 *
 * Quand activé, lance un AP WiFi "HEELPMEE-Config" et un serveur HTTP
 * sur 192.168.4.1. L'utilisateur s'y connecte avec son téléphone pour :
 *   - Voir l'état du device (battery, signal, FSM, IP)
 *   - Configurer le WiFi de fallback (SSID/password)
 *   - Configurer le broker MQTT et les topics
 *   - Lister et télécharger les fichiers audio sur la SD
 *   - Redémarrer le device
 *
 * Le portail s'éteint automatiquement après CAPTIVE_TIMEOUT_MS d'inactivité,
 * ou sur appel de captivePortal_stop().
 */
#ifndef CAPTIVE_PORTAL_H
#define CAPTIVE_PORTAL_H

#include <Arduino.h>
#include <SPI.h>
extern SPIClass sdSpi;

// Démarre le portail (AP WiFi + serveur HTTP)
bool captivePortal_start();

// Arrête le portail proprement (libère WiFi, ferme serveur)
void captivePortal_stop();

// À appeler dans loop() — gère les requêtes HTTP et le timeout
void captivePortal_loop();

// État
bool captivePortal_isActive();

// Reset le timer d'inactivité (appelé à chaque requête HTTP reçue)
void captivePortal_kickWatchdog();

#endif