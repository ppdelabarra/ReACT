#include "wind_sensor.h"
#include <Adafruit_ADS1X15.h>

extern Adafruit_ADS1115 ads;

int WindSensor::readADC() {
    return ads.readADC_SingleEnded(0);
}