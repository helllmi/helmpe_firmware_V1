#include <Arduino.h>
#include "sos_button.h"
#include "config.h"
#include "led.h"

// État debounced du bouton (LOW = appuyé)
static bool buttonState = HIGH;   // état stable après debounce
static bool lastReading = HIGH;   // dernière lecture brute
static uint32_t lastBounceMs = 0; // moment du dernier changement brut

// État pour le long press
static bool longPressActive = false;    // appui long en cours (déjà signalé)
static bool longPressEvent  = false;    // flag à lire avec sosButton_wasLongPressed()

// Compteur de clics dans la fenêtre actuelle
static uint8_t clickCount = 0;
static uint32_t firstClickMs = 0; // timestamp du 1er clic d'une séquence
static uint32_t lastClickMs = 0;  // timestamp du dernier clic
static uint32_t pressStartMs = 0; // début du clic/appui en cours (servait au long press ET au calcul de durée)

// Drapeau "triple clic détecté"
static bool tripleClickFlag = false;

// Données du dernier triple clic (pour le JSON)
static uint8_t lastPressCount = 0;
static uint32_t lastPressDuration = 0;

// ============================================================================
//  INIT
// ============================================================================
void sosButton_init()
{
    pinMode(PIN_SOS_BUTTON, INPUT_PULLUP);
    Serial.printf("[BTN] Init detector on GPIO %d\n", PIN_SOS_BUTTON);
    Serial.printf("[BTN] Window=%dms, Debounce=%dms, ClickMax=%dms, LongPress=%dms\n",
                  SOS_TRIPLE_WINDOW_MS, SOS_DEBOUNCE_MS, SOS_CLICK_MAX_MS, LONG_PRESS_MS);
}

// ============================================================================
//  TICK — à appeler depuis loop()
// ============================================================================
void sosButton_tick()
{
    uint32_t now = millis();
    bool reading = digitalRead(PIN_SOS_BUTTON);

    // ── 1) ANTI-REBOND : on attend SOS_DEBOUNCE_MS de stabilité ─────────────
    if (reading != lastReading)
    {
        lastBounceMs = now; // un changement vient d'arriver, on reset le timer
        lastReading = reading;
        // Pas de return ici : on doit aussi vérifier le long press en cours
    }

    // ── 2) DÉTECTION DU LONG PRESS PENDANT L'APPUI ──────────────────────────
    // Cette détection se fait MÊME pendant le debounce du relâchement,
    // car on veut savoir si le bouton est maintenu pressé depuis ≥ LONG_PRESS_MS.
    if (buttonState == LOW && !longPressActive && pressStartMs > 0)
    {
        uint32_t held = now - pressStartMs;
        if (held >= LONG_PRESS_MS)
        {
            longPressActive = true;
            longPressEvent = true;
            Serial.printf("[BTN] *** LONG PRESS DETECTED (%lums) ***\n", held);

            // Feedback visuel : 1 long flash rouge
            blinkLED(LED_ERROR, 1, 500);

            // On annule toute séquence de clic en cours (cohérent : c'est un long press, pas un clic)
            clickCount = 0;
            firstClickMs = 0;
        }
    }

    // Si on n'a pas encore atteint la stabilité, on attend (pour les autres détections)
    if ((now - lastBounceMs) < SOS_DEBOUNCE_MS)
    {
        return;
    }

    // Si l'état stable n'a pas changé, rien à faire
    if (reading == buttonState)
    {
        return;
    }

    // ── 3) UN CHANGEMENT STABLE A EU LIEU ───────────────────────────────────
    buttonState = reading;

    // ── 4) FRONT DESCENDANT (HIGH→LOW) : appui détecté ──────────────────────
    if (buttonState == LOW)
    {
        pressStartMs = now;
        longPressActive = false; // reset le flag long press pour ce nouvel appui

        // Si c'est le tout premier clic, ou si la fenêtre est expirée → nouveau cycle
        if (clickCount == 0 || (now - firstClickMs) > SOS_TRIPLE_WINDOW_MS)
        {
            clickCount = 1;
            firstClickMs = now;
            Serial.println("[BTN] Click 1/3 (start window)");
        }
        else
        {
            clickCount++;
            Serial.printf("[BTN] Click %d/3\n", clickCount);
        }

        lastClickMs = now;
    }

    // ── 5) FRONT MONTANT (LOW→HIGH) : relâchement détecté ────────────────────
    else
    {
        uint32_t pressDuration = now - pressStartMs;

        // Si c'était un long press déjà signalé → on ignore ce relâchement
        // (le long press a déjà fait son boulot, pas la peine de le compter en clic)
        if (longPressActive)
        {
            Serial.printf("[BTN] Long press released (was %dms)\n", pressDuration);
            longPressActive = false;
            clickCount = 0;
            firstClickMs = 0;
            pressStartMs = 0;
            return;
        }

        // Si l'appui a duré trop longtemps pour un clic mais pas assez pour long press → on ignore
        if (pressDuration > SOS_CLICK_MAX_MS)
        {
            Serial.printf("[BTN] Press too long for click, too short for long press (%dms)\n", pressDuration);
            clickCount = 0;
            return;
        }

        // ── 6) TRIPLE CLIC COMPLET ? ────────────────────────────────────────
        if (clickCount >= 3 && (now - firstClickMs) <= SOS_TRIPLE_WINDOW_MS)
        {
            Serial.println("[BTN] *** TRIPLE CLICK DETECTED ***");

            // Capture des stats pour le JSON
            lastPressCount = clickCount;
            lastPressDuration = now - firstClickMs;
            tripleClickFlag = true;

            // Feedback visuel : 3 flashs rouges rapides
            blinkLED(LED_ERROR, 3, 100);

            // Reset pour le prochain triple clic
            clickCount = 0;
            firstClickMs = 0;
        }

        pressStartMs = 0; // appui terminé
    }
}

// ============================================================================
//  API DE LECTURE
// ============================================================================
bool sosButton_wasTripleClicked()
{
    if (tripleClickFlag)
    {
        tripleClickFlag = false; // auto-reset (latch consumable)
        return true;
    }
    return false;
}

bool sosButton_wasLongPressed()
{
    if (longPressEvent)
    {
        longPressEvent = false; // auto-reset
        return true;
    }
    return false;
}

bool sosButton_isPressed()
{
    return buttonState == LOW;
}

uint32_t sosButton_getCurrentPressDurationMs()
{
    if (pressStartMs == 0 || buttonState == HIGH) return 0;
    return millis() - pressStartMs;
}

uint8_t sosButton_getPressCount()
{
    return lastPressCount;
}

uint32_t sosButton_getPressDurationMs()
{
    return lastPressDuration;
}