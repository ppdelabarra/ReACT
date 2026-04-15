#include "opt3001_sensor.h"

void OPT3001Sensor::begin() {
    sensor.begin(0x45);

    OPT3001_Config newConfig;
    newConfig.RangeNumber = B1100;
    newConfig.ConvertionTime = B0;
    newConfig.Latch = B1;
    newConfig.ModeOfConversionOperation = B11; // Continuous mode

    sensor.writeConfig(newConfig);
}

float OPT3001Sensor::readLux() {
    OPT3001 result = sensor.readResult();

    if (result.error == NO_ERROR) {
        return result.lux;
    }
    return -1;
}
