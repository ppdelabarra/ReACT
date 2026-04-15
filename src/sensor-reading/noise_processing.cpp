#include "noise_processing.h"
#include <math.h>

NoiseData processNoise(float *samples, int n) {

    NoiseData d;

    float sum = 0, sumSq = 0;

    for (int i = 0; i < n; i++) {
        sum += samples[i];
        sumSq += samples[i] * samples[i];
    }

    float mean = sum / n;
    float var = (sumSq / n) - (mean * mean);

    if (var < 0) var = 0;

    d.vrms = sqrt(var);

    return d;
}