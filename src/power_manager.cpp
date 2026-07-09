#include <Arduino.h>
#include "power_manager.h"
#include "config.h"
#include <WiFi.h>
#include "esp_sleep.h"
#include "driver/uart.h"

// ============================================================================
//  ÉTAT INTERNE
// ============================================================================
static uint32_t currentFreq = CPU_FREQ_BOOT_MHZ;

// ============================================================================
//  INIT
// ============================================================================
void power_init()
{
    Serial.println("[PWR] Init power manager");

    // Au boot, on tourne à pleine puissance pour une init rapide
    power_setcpufreq(CPU_FREQ_BOOT_MHZ);

    Serial.printf("[PWR] CPU freq = %u MHz\n", currentFreq);
}

// ============================================================================
//  CPU FREQUENCY SCALING
// ============================================================================
void power_setcpufreq(uint32_t mhz)
{
    // setCpuFrequencyMhz() est fournie par le core Arduino ESP32
    bool ok = setCpuFrequencyMhz(mhz);
    if (ok)
    {
        currentFreq = mhz;
        Serial.printf("[PWR] CPU freq set to %u MHz\n", mhz);
    }
    else
    {
        Serial.printf("[PWR] FAILED to set CPU freq to %u MHz\n", mhz);
    }
}

uint32_t power_getcpufreq()
{
    return currentFreq;
}

// ============================================================================
//  MODE STANDBY — économie d'énergie
// ============================================================================
void power_enterstandby()
{
    Serial.println("[PWR] Entering STANDBY power mode");

    // 1) Réduire la fréquence CPU
    power_setcpufreq(CPU_FREQ_STANDBY_MHZ);

    // 2) Activer le WiFi modem-sleep (si WiFi connecté)
    //    Le modem WiFi dort entre les beacons de l'AP, se réveille pour
    //    les recevoir. Économie sans perdre la connexion.
    if (WiFi.status() == WL_CONNECTED)
    {
        WiFi.setSleep(WIFI_PS_MIN_MODEM);
        Serial.println("[PWR] WiFi modem-sleep enabled");
    }
}

// ============================================================================
//  MODE ACTION — pleine puissance
// ============================================================================
void power_enteraction()
{
    Serial.println("[PWR] Entering ACTION power mode (full power)");

    // 1) Pleine fréquence CPU pour réactivité maximale
    power_setcpufreq(CPU_FREQ_ACTION_MHZ);

    // 2) Désactiver le WiFi sleep : on veut le débit maximal pour
    //    transmettre les alertes le plus vite possible
    if (WiFi.status() == WL_CONNECTED)
    {
        WiFi.setSleep(WIFI_PS_NONE);
        Serial.println("[PWR] WiFi sleep disabled (full throughput)");
    }
}
void power_lightSleep(uint32_t durationMs) {
    // Configurer le réveil sur timer
    esp_sleep_enable_timer_wakeup((uint64_t)durationMs * 1000ULL);  // µs

    // Configurer le réveil sur le bouton SOS (GPIO 7, niveau bas = pressé)
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_SOS_BUTTON, 0);  // 0 = LOW

    // Configurer le réveil sur l'UART du modem (UART1, données entrantes)
    // Nécessaire pour ne pas rater les URC +CMQTTRXSTART (notifications OTA)
    uart_set_wakeup_threshold(UART_NUM_1, 3);  // réveil après 3 caractères
    esp_sleep_enable_uart_wakeup(UART_NUM_1);

    // Aller dormir
    esp_light_sleep_start();

    // Réveil ici. On peut éventuellement logger la cause :
    // esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    // (pas affiché pour éviter de spammer)
}