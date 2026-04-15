#include "max31865_sensor.h"

MAX31865Sensor::MAX31865Sensor(int cs, int mosi, int miso, int sck)
: sensor(cs, mosi, miso, sck) {}

void MAX31865Sensor::begin() {
    sensor.begin(MAX31865_4WIRE);
}

float MAX31865Sensor::readTemperature() {
    return sensor.temperature(rtdNominal, refResistor);
}

uint8_t MAX31865Sensor::readFault() {
    return sensor.readFault();
}

void MAX31865Sensor::clearFault() {
    sensor.clearFault();
}