/**
 * audio.h — Capture audio INMP441 vers carte SD
 *
 * Capture 60s d'audio à l'entrée en STATE_ACTION et sauvegarde un fichier
 * WAV séquentiel sur la carte SD (alert_001.wav, alert_002.wav, ...).
 *
 * La capture tourne dans une tâche FreeRTOS séparée pour ne pas bloquer
 * la loop principale (FSM, MQTT, GPS continuent à fonctionner).
 */
#ifndef AUDIO_H
#define AUDIO_H

#include <Arduino.h>

bool audio_init();

bool audio_startRecording();

void audio_stopRecording();

bool audio_isRecording();

uint32_t audio_getLastSeq();

// Récupère le chemin complet du dernier fichier (pour logs / inclusion JSON)
const char *audio_getLastFilePath();

#endif