#ifndef CONFIG_H
#define CONFIG_H

// ================= WIND SENSOR (Rev P model) =================
#define WIND_ADC_OFFSET 264.0
#define WIND_SCALE      85.6814
#define WIND_EXPONENT   3.36814

// ================= NOISE SENSOR (SEN0232) =================
#define NOISE_REF_VRMS  0.01
#define NOISE_DB_OFFSET 60.0

// ================= LIGHT SENSOR =================
#define LUX_GAIN   1.10
#define LUX_OFFSET 0.0

// ================= GLobe TEMP =================
#define GLOBE_TEMP_OFFSET 0.0

// ================= CO2 calibration =================
#define CO2_FRC_VALUE 400

#endif