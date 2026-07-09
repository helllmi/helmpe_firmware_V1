#include <Arduino.h>
#include <Preferences.h>
#include "jwt_storage.h"
#include "config.h"

static Preferences prefs;
static String currentToken;

void jwtStorage_load()
{
    if (!prefs.begin(NVS_NAMESPACE_JWT, false)) {
        Serial.println("[JWT] NVS open failed, using config.h fallback");
        currentToken = JWT_TOKEN_FALLBACK;
        return;
    }

    String stored = prefs.getString(NVS_KEY_JWT_TOKEN, "");
    prefs.end();

    if (stored.length() > 0) {
        currentToken = stored;
        Serial.printf("[JWT] Token loaded from NVS (%u chars)\n", stored.length());
    } else {
        currentToken = JWT_TOKEN_FALLBACK;
        Serial.println("[JWT] No token in NVS, using config.h fallback");
    }
}

bool jwtStorage_save(const String& token)
{
    if (!prefs.begin(NVS_NAMESPACE_JWT, false)) {
        Serial.println("[JWT] NVS open failed for writing");
        return false;
    }

    size_t written = prefs.putString(NVS_KEY_JWT_TOKEN, token);
    prefs.end();

    if (written == 0) {
        Serial.println("[JWT] Write returned 0 bytes (FAILED)");
        return false;
    }

    // Vérification par relecture
    prefs.begin(NVS_NAMESPACE_JWT, true);
    String verify = prefs.getString(NVS_KEY_JWT_TOKEN, "");
    prefs.end();

    if (verify != token) {
        Serial.println("[JWT] Write verification FAILED (readback mismatch)");
        return false;
    }

    currentToken = token;
    Serial.printf("[JWT] Token saved and verified (%u bytes)\n", written);
    return true;
}

const String& jwtStorage_get()
{
    return currentToken;
}