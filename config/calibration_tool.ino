// ================= INCLUDES =================
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <Adafruit_MAX31865.h>
#include <SparkFun_SCD4x_Arduino_Library.h>
#include <ClosedCube_OPT3001.h>
#include <math.h>

// Network & OTA Includes
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>

// ================= GITHUB / OTA CONFIG =================
const char* version_url = "https://raw.githubusercontent.com/ppdelabarra/ReACT/main/version.json";

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

// ================= MENU & TEST STATE =================
int currentMode = 0; 
unsigned long lastReadTime = 0;
const unsigned long READ_INTERVAL = 2000; 

// ================= CALIBRATION TEST VARIABLES =================
float test_wind_zero = 264.0;
float test_wind_scale = 85.6814;
float test_wind_exp = 3.36814;
float test_temp_exp = 0.5;

float test_noise_offset = 0.0;

// ================= OPT3001 CONFIG =================
void configureOPT3001() {
  OPT3001_Config cfg;
  cfg.RangeNumber = B1100;
  cfg.ConvertionTime = B0;
  cfg.Latch = B1;
  cfg.ModeOfConversionOperation = B11;
  opt3001.writeConfig(cfg);
}

// ================= MENU =================
void printMenu() {
  Serial.println("\n=============================================");
  Serial.println("         SENSOR CALIBRATION TOOL");
  Serial.println("=============================================");
  Serial.println("[1] Wind Sensor (RAW + CAL MODE)");
  Serial.println("[2] Noise Sensor");
  Serial.println("[3] Light Sensor");
  Serial.println("[4] SCD41");
  Serial.println("[5] Globe Temp");
  Serial.println("[6] CO2 Calibration");
  Serial.println("[7] Exit + OTA");
  Serial.println("[0] Menu");
  Serial.println("---------------------------------------------");
  Serial.println("Commands:");
  Serial.println("W<number> -> set wind zero");
  Serial.println("S<number> -> set wind scale");
  Serial.println("E<number> -> set exponent");
  Serial.println("T<number> -> set temp exponent");
  Serial.println("=============================================");
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n=== CALIBRATION MODE ===");

  Wire.begin(SDA_PIN, SCL_PIN);

  scd41.begin(Wire);
  scd41.startPeriodicMeasurement();

  opt3001.begin(OPT3001_ADDRESS);
  configureOPT3001();

  ads.begin();
  ads.setGain(GAIN_ONE);

  max31865.begin(MAX31865_4WIRE);

  printMenu();
}

// ================= LOOP =================
void loop() {

  // ===== SERIAL INPUT =====
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();

    if (input.startsWith("W")) {
      test_wind_zero = input.substring(1).toFloat();
    } else if (input.startsWith("S")) {
      test_wind_scale = input.substring(1).toFloat();
    } else if (input.startsWith("E")) {
      test_wind_exp = input.substring(1).toFloat();
    } else if (input.startsWith("T")) {
      test_temp_exp = input.substring(1).toFloat();
    } else if (input.length() == 1) {
      currentMode = input.toInt();
      if (currentMode == 0) printMenu();
    }
  }

  // ===== SENSOR LOOP =====
  if (millis() - lastReadTime > READ_INTERVAL) {
    lastReadTime = millis();

    switch (currentMode) {

      // ================= WIND CALIBRATION =================
      case 1: {

        // --- WIND SIGNAL ---
        int16_t adc0 = ads.readADC_SingleEnded(0);
        float voltage0 = adc0 * 0.000125;
        float simADC = (voltage0 / 5.0) * 1024.0;

        // --- TMP ---
        int16_t adc1 = ads.readADC_SingleEnded(1);
        float voltageTMP = adc1 * 0.000125;
        float tempTMP = (voltageTMP - 0.400) / 0.0195;

        // --- MODEL ---
        float x = (simADC - test_wind_zero) / test_wind_scale;
        float windMPH = (x < 0) ? 0 : pow(x, test_wind_exp);

        float tempFactor = pow((tempTMP + 273.15) / (25.0 + 273.15), test_temp_exp);
        windMPH *= tempFactor;

        float windMS = windMPH * 0.44704;

        // ===== OUTPUT =====

        // Human readable
        Serial.printf(
          "Wind | V=%.3f | ADC=%.1f | TMP=%.2f°C | Speed=%.2f m/s\n",
          voltage0, simADC, tempTMP, windMS
        );

        // 🔥 CALIBRATION LINE (for dataset building)
        Serial.printf(
          "CAL,%.2f,%.2f,%.3f\n",
          simADC, tempTMP, windMS
        );

        // 🔥 RAW LINE (for advanced calibration)
        Serial.printf(
          "RAW,%.3f,%.3f\n",
          voltage0, voltageTMP
        );

        break;
      }

      // ================= NOISE =================
      case 2: {
        int16_t adc2 = ads.readADC_SingleEnded(2);
        float v = adc2 * 0.000125;
        float db = (v * 50.0) + test_noise_offset;
        Serial.printf("Noise | V=%.3f | dBA=%.1f\n", v, db);
        break;
      }

      // ================= LIGHT =================
      case 3: {
        OPT3001 result = opt3001.readResult();
        Serial.printf("Light | Lux=%.2f\n", result.lux);
        break;
      }

      // ================= SCD41 =================
      case 4: {
        if (scd41.readMeasurement()) {
          Serial.printf("Temp=%.2f | Hum=%.1f | CO2=%d\n",
                        scd41.getTemperature(),
                        scd41.getHumidity(),
                        scd41.getCO2());
        }
        break;
      }

      // ================= GLOBE =================
      case 5: {
        float t = max31865.temperature(RNOMINAL, RREF);
        Serial.printf("Globe Temp=%.2f\n", t);
        break;
      }
    }
  }
}
