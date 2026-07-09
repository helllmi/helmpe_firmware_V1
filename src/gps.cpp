#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include "gps.h"
#include "serial_comm.h"

GPSData currentGPS;

// ============================================================================
//  LECTURE GPS via SIM7670G (AT+CGNSSINFO)
// ============================================================================

bool readGPS() {
    // Ne pas lire le GPS si le modem est verrouillé (upload en cours)
    if (!modemMutex_tryTake()) {
        Serial.println("[GPS] Modem busy, skipping GPS read");
        return false;
    }

    while (ss.available()) ss.read();

    ss.println("AT+CGNSSINFO");

    String resp = waitForResponse("OK", 3000);

    int idx = resp.indexOf("+CGNSSINFO:");
    if (idx == -1) {
        modemMutex_give();
        return false;
    }

    String data = resp.substring(idx + 12);
    data.trim();

    String parts[20];
    int partIndex = 0;
    int lastComma = 0;

    for (int i = 0; i <= (int)data.length() && partIndex < 20; i++) {
        if (i == (int)data.length() || data[i] == ',') {
            parts[partIndex++] = data.substring(lastComma, i);
            lastComma = i + 1;
        }
    }

    if (parts[0].toInt() < 2) {
        currentGPS.valid = false;
        modemMutex_give();
        return false;
    }

    if (parts[5].length() == 0 || parts[7].length() == 0) {
        currentGPS.valid = false;
        modemMutex_give();
        return false;
    }

    currentGPS.latitude  = parts[5].toDouble();
    currentGPS.longitude = parts[7].toDouble();

    if (parts[6] == "S") currentGPS.latitude  *= -1;
    if (parts[8] == "W") currentGPS.longitude *= -1;

    currentGPS.altitude = parts[10].toDouble();
    currentGPS.speed    = parts[11].toDouble();
    currentGPS.valid    = true;

    modemMutex_give();
    return true;
}

void logGPS() {
    if (!currentGPS.valid) return;

    File f = SD.open("/gps_log.csv", FILE_APPEND);
    if (f) {
        f.printf("%.6f,%.6f,%.2f\n",
                 currentGPS.latitude,
                 currentGPS.longitude,
                 currentGPS.altitude);
        f.close();
    }
}