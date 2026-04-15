#include "calibration.h"
#include "config.h"
#include <math.h>

// ================= WIND (IMPORTANT FIX) =================
// Uses REAL reference equation (Rev P sensor)
float calibrateWind(int adc) {

    float x = (adc - WIND_ADC_OFFSET) / WIND_SCALE;

    if (x < 0) return 0;

    float wind = pow(x, WIND_EXPONENT);

    return wind;
}

// ================= NOISE (SPL CALIBRATION) =================
float calibrateNoise(float vrms) {
    return 20.0 * log10((vrms / NOISE_REF_VRMS) + 1e-9) + NOISE_DB_OFFSET;
}

// ================= LIGHT =================
float calibrateLux(float lux) {
    return lux * LUX_GAIN + LUX_OFFSET;
}

// ================= GLOBE TEMP =================
float calibrateGlobeTemp(float t) {
    return t + GLOBE_TEMP_OFFSET;
}