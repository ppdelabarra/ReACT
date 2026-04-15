#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include "scd41_sensor.h"
#include "opt3001_sensor.h"
#include "noise_sensor.h"
#include "noise_processing.h"
#include "max31865_sensor.h"

SCD41Sensor scd;
OPT3001Sensor light;
NoiseSensor noise;
Adafruit_ADS1115 ads;

#define MAX_CS 5
#define MAX_MOSI 23
#define MAX_MISO 19
#define MAX_SCK 18
MAX31865Sensor globe(MAX_CS, MAX_MOSI, MAX_MISO, MAX_SCK);

float samples[100];

void setup() {
  Serial.begin(115200);
  Wire.begin(21,22);

  scd.begin(Wire);
  light.begin();
  ads.begin();
  ads.setGain(GAIN_FOUR);
  globe.begin();
}

void loop() {

  Serial.println("=== CALIBRATION MODE ===");

  int adc = ads.readADC_SingleEnded(0);
  float v = adc * 0.0000625;
  Serial.print("Wind ADC: "); Serial.print(adc);
  Serial.print(" Voltage: "); Serial.println(v);

  noise.sample(samples,100);
  NoiseData nd = processNoise(samples,100);
  Serial.print("Noise VRMS: "); Serial.println(nd.vrms);

  float lux = light.readLux();
  Serial.print("Lux raw: "); Serial.println(lux);

  SCD41Data d;
  scd.read(d);
  Serial.print("Temp raw: "); Serial.println(d.temp);
  Serial.print("Hum raw: "); Serial.println(d.hum);

  float globeT = globe.readTemperature();
  Serial.print("Globe raw: "); Serial.println(globeT);

  Serial.println("-----------------------");

  delay(3000);
}
