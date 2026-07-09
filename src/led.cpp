#include <Adafruit_NeoPixel.h>
#include "led.h"

Adafruit_NeoPixel strip = Adafruit_NeoPixel(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// État interne du heartbeat
static uint32_t lastHeartbeatMs = 0;
static bool     heartbeatOn     = false;

// ============================================================================
//  INIT & PRIMITIVES
// ============================================================================

void ledBegin() {
    strip.begin();
    strip.setBrightness(100);
    strip.show();
}

void setLED(uint32_t color) {
    strip.setPixelColor(0, color);
    strip.show();
}

void blinkLED(uint32_t color, int times, int delayMs) {
    for (int i = 0; i < times; i++) {
        strip.setPixelColor(0, color);
        strip.show();
        delay(delayMs);
        strip.setPixelColor(0, LED_OFF);
        strip.show();
        delay(delayMs);
    }
}

// ============================================================================
//  HEARTBEAT NON-BLOQUANT (pour la fenêtre d'observation post-OTA, idle, etc.)
// ============================================================================
void ledHeartbeatTick(uint32_t color, uint32_t intervalMs) {
    uint32_t now = millis();
    if (now - lastHeartbeatMs >= intervalMs) {
        lastHeartbeatMs = now;
        heartbeatOn = !heartbeatOn;
        strip.setPixelColor(0, heartbeatOn ? color : LED_OFF);
        strip.show();
    }
}
