#ifndef SCD41_SENSOR_H
#define SCD41_SENSOR_H

#include <SparkFun_SCD4x_Arduino_Library.h>

struct SCD41Data {
    float temp;
    float hum;
    uint16_t co2;
};

class SCD41Sensor {
public:
    bool begin(TwoWire &wire);
    bool read(SCD41Data &data);

private:
    SCD4x sensor;
};

#endif