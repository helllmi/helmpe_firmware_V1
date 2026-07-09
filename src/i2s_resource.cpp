#include "i2s_resource.h"

static SemaphoreHandle_t i2sMutex = NULL;

void i2sResource_init()
{
    i2sMutex = xSemaphoreCreateMutex();
    configASSERT(i2sMutex != NULL);
    Serial.println("[I2S-RES] Mutex created");
}

bool i2sResource_take()
{
    if (i2sMutex == NULL) {
        Serial.println("[I2S-RES] ERROR: mutex not initialized");
        return false;
    }
    bool ok = (xSemaphoreTake(i2sMutex, pdMS_TO_TICKS(5000)) == pdTRUE);
    if (!ok) Serial.println("[I2S-RES] ERROR: mutex take timeout (5s)");
    return ok;
}

void i2sResource_give()
{
    if (i2sMutex != NULL) {
        xSemaphoreGive(i2sMutex);
    }
}