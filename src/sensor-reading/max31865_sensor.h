#ifndef MAX31865_SENSOR_H
#define MAX31865_SENSOR_H

#include <Adafruit_MAX31865.h>

class MAX31865Sensor {
public:
    MAX31865Sensor(int cs, int mosi, int miso, int sck);

    void begin();
    float readTemperature();
    uint8_t readFault();
    void clearFault();

private:
    Adafruit_MAX31865 sensor;

    float rtdNominal = 100.0;
    float refResistor = 430.0;
};

#endif