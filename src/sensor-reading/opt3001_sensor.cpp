#include "opt3001_sensor.h"

void OPT3001Sensor::begin() {
    sensor.begin(0x45);
}

float OPT3001Sensor::readLux() {
    OPT3001 result = sensor.readResult();

    if (result.error == NO_ERROR) {
        return result.lux;
    }
    return -1;
}