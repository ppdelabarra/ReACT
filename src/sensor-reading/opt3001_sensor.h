#ifndef OPT3001_SENSOR_H
#define OPT3001_SENSOR_H

#include <ClosedCube_OPT3001.h>

class OPT3001Sensor {
public:
    void begin();
    float readLux();

private:
    ClosedCube_OPT3001 sensor;
};

#endif