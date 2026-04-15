#include "noise_processing.h"
#include <math.h>

NoiseData processNoise(float *samples, int n) {

    NoiseData d;

    float sumSq = 0;

    for (int i = 0; i < n; i++) {
        sumSq += samples[i] * samples[i];
    }

    d.vrms = sqrt(sumSq / n);

    if (isnan(d.vrms) || isinf(d.vrms)) {
        d.vrms = 0;
    }

    return d;
}
