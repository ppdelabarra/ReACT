#include "noise_sensor.h"
#include <Adafruit_ADS1X15.h>

extern Adafruit_ADS1115 ads;

void NoiseSensor::sample(float *buffer, int n) {
    for (int i = 0; i < n; i++) {
        int16_t adc = ads.readADC_SingleEnded(1);
        buffer[i] = adc * 0.0000625;
        delayMicroseconds(200);
    }
}