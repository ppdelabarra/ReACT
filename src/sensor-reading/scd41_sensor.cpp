#include "scd41_sensor.h"

bool SCD41Sensor::begin(TwoWire &wire) {
    if (sensor.begin(wire)) {
        sensor.startPeriodicMeasurement();
        return true;
    }
    return false;
}

bool SCD41Sensor::read(SCD41Data &data) {
    if (!sensor.getDataReadyStatus()) return false;

    if (sensor.readMeasurement()) {
        data.temp = sensor.getTemperature();
        data.hum  = sensor.getHumidity();
        data.co2  = sensor.getCO2();
        return true;
    }
    return false;
}