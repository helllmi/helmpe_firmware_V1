#ifndef GPS_H
#define GPS_H

#include <Arduino.h>

struct GPSData {
    double latitude  = 0.0;
    double longitude = 0.0;
    double altitude  = 0.0;
    double speed     = 0.0;
    bool   valid     = false;
};

extern GPSData currentGPS;

bool readGPS();
void logGPS();

#endif
