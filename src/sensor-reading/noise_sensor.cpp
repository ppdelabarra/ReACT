#include "noise_sensor.h"
#include <Adafruit_ADS1X15.h>

extern Adafruit_ADS1115 ads;

void NoiseSensor::sample(float *buffer, int n) {

    float baseline = 0;

    // 1) collect raw samples
    for (int i = 0; i < n; i++) {
        int16_t adc = ads.readADC_SingleEnded(1);
        buffer[i] = ads.computeVolts(adc);  // FIX: proper scaling
        delayMicroseconds(200);
    }

    // 2) compute DC offset
    for (int i = 0; i < n; i++) {
        baseline += buffer[i];
    }
    baseline /= n;

    // 3) remove DC offset
    for (int i = 0; i < n; i++) {
        buffer[i] -= baseline;
    }
}
