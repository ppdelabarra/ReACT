#ifndef NOISE_PROCESSING_H
#define NOISE_PROCESSING_H

struct NoiseData {
    float vrms;
};

NoiseData processNoise(float *samples, int n);

#endif