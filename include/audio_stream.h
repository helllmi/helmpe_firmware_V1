#ifndef AUDIO_STREAM_H
#define AUDIO_STREAM_H

#include <Arduino.h>

void audioStream_init();

// Appelées depuis le callback MQTT (main task) — ne font que poser un flag.
void audioStream_requestStart(const char *streamId);
void audioStream_requestStop();

// À appeler dans loop() (main task) : publie l'état MQTT de façon thread-safe.
void audioStream_tick();

bool audioStream_isActive();

#endif // AUDIO_STREAM_H