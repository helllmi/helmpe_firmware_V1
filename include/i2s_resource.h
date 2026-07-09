#ifndef I2S_RESOURCE_H
#define I2S_RESOURCE_H

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// ============================================================================
//  I2S_RESOURCE — mutex de protection de I2S_NUM_0 (ressource partagée)
// ============================================================================
//  I2S_NUM_0 est utilisé par deux modules :
//    - voice_trigger.cpp (écoute continue en STANDBY)
//    - audio.cpp         (enregistrement WAV en ACTION)
//
//  Ces deux modules ne tournent jamais en même temps par conception FSM,
//  mais un mutex garantit qu'aucune lecture I2S résiduelle (i2s_read bloquant)
//  ne chevauche l'init/deinit de l'autre module.
//
// ============================================================================


void i2sResource_init();


bool i2sResource_take();


void i2sResource_give();

#endif