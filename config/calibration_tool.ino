// ================= INCLUDES =================
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <Adafruit_MAX31865.h>
#include <SparkFun_SCD4x_Arduino_Library.h>
#include <ClosedCube_OPT3001.h>

// ================= I2C =================
#define SDA_PIN 21
#define SCL_PIN 22

// ================= SENSORS =================
#define OPT3001_ADDRESS 0x45
ClosedCube_OPT3001 opt3001;

#define MAX_CS   5
#define MAX_MOSI 23
#define MAX_MISO 19
#define MAX_SCK  18
#define RREF      430.0
#define RNOMINAL  100.0
Adafruit_MAX31865 max31865(MAX_CS, MAX_MOSI, MAX_MISO, MAX_SCK);

SCD4x scd41;
Adafruit_ADS1115 ads;

// ================= OPT3001 CONFIG =================
void configureOPT3001() {
  OPT3001_Config cfg;
  cfg.RangeNumber = B1100;
  cfg.ConvertionTime = B0;
  cfg.Latch = B1;
  cfg.ModeOfConversionOperation = B11;
  opt3001.writeConfig(cfg);
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n=== SYSTEM BOOT: CALIBRATION MODE ===");

  Wire.begin(SDA_PIN, SCL_PIN);

  if (scd41.begin(Wire)) {
    Serial.println("SCD41 detected ✅");
    scd41.startPeriodicMeasurement();
  }
  
  opt3001.begin(OPT3001_ADDRESS);
  configureOPT3001();
  Serial.println("OPT3001 configured ✅");

  if (ads.begin()) {
    Serial.println("ADS1115 detected ✅");
    ads.setGain(GAIN_ONE);  // FIX: Allows reading up to +/- 4.096V
  }

  max31865.begin(MAX31865_4WIRE);
  Serial.println("MAX31865 initialized ✅");
  Serial.println("================================\n");
}

// ================= LOOP =================
void loop() {
  Serial.println("=== RAW CALIBRATION DATA ===");

  // 1. WIND SENSOR (ADC0)
  int16_t adc0 = ads.readADC_SingleEnded(0);
  float voltage0 = adc0 * 0.000125; // GAIN_ONE multiplier
  float simulatedArduinoADC = (voltage0 / 5.0) * 1024.0;
  Serial.printf("Wind -> Raw Volts: %.3f V | Sim ADC: %.1f\n", voltage0, simulatedArduinoADC);

  // 2. NOISE SENSOR (ADC1)
  int16_t adc1 = ads.readADC_SingleEnded(1);
  float voltage1 = adc1 * 0.000125; 
  if (voltage1 < 0) voltage1 = 0;
  float rawDBA = voltage1 * 50.0;
  Serial.printf("Noise -> Raw Volts: %.3f V | Calc dBA: %.1f\n", voltage1, rawDBA);

  // 3. LIGHT SENSOR
  OPT3001 result = opt3001.readResult();
  if (result.error == NO_ERROR) {
    Serial.printf("Light -> Raw Lux: %.2f\n", result.lux);
  } else {
    Serial.println("Light -> Read Error");
  }

  // 4. SCD41 (TEMP, HUM, CO2)
  if (scd41.getDataReadyStatus() && scd41.readMeasurement()) {
    Serial.printf("SCD41 -> Raw Temp: %.2f °C | Raw Hum: %.1f %% | Raw CO2: %d ppm\n", 
                  scd41.getTemperature(), scd41.getHumidity(), scd41.getCO2());
  } else {
    Serial.println("SCD41 -> Not ready");
  }

  // 5. MAX31865 (GLOBE TEMP)
  float globeTemp = max31865.temperature(RNOMINAL, RREF);
  Serial.printf("Globe -> Raw Temp: %.2f °C\n", globeTemp);

  Serial.println("----------------------------\n");

  delay(3000);
}
