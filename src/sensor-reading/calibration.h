#ifndef CALIBRATION_H
#define CALIBRATION_H

float calibrateWind(int adc);
float calibrateNoise(float vrms);
float calibrateLux(float lux);
float calibrateGlobeTemp(float t);

#endif