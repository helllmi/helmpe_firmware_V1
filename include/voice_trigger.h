#ifndef VOICE_TRIGGER_H
#define VOICE_TRIGGER_H

#include <Arduino.h>

// ============================================================================
//  VOICE_TRIGGER — détection du mot "Sos" via Edge Impulse (inférence continue)
// ============================================================================
//  Utilise le modèle Alert-helpmee_inferencing (4 classes : Sos, helloworld,
//  noise, unknown) pour écouter en permanence pendant STATE_STANDBY.
//
//  Partage I2S_NUM_0 avec audio.cpp — ne tourne JAMAIS en même temps que
//  l'enregistrement WAV. voiceTrigger_stop() doit être appelé avant
//  audio_startRecording(), et voiceTrigger_start() après audio_stopRecording().
//
//  Quand "Sos" est détecté avec un score >= VOICE_TRIGGER_THRESHOLD,
//  dispatch EVT_SOS_TRIGGERED directement depuis la tâche FreeRTOS.
// ============================================================================

#define VOICE_TRIGGER_THRESHOLD 0.4f

void voiceTrigger_start();

void voiceTrigger_stop();

bool voiceTrigger_isRunning();

#endif