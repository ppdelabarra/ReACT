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

// ================= WIND CALIBRATION =================
// Match the dedicated wind calibration sketch
const float REG_A = 3.038517f;
const float REG_B = 0.115157f;
const float REG_C = 3.009364f;
const float REG_D = 0.087288f;

float test_wind_zero = 1.346f;   // zero-wind OUT voltage
bool autoZeroMode = false;
float zeroAccumulator = 0.0f;
int zeroSamplesCollected = 0;
int zeroSamplesTarget = 100;

float test_noise_offset = 0.0f;

// ================= HELPERS =================
float adcToVolts(int16_t raw) {
  // ADS1115 at GAIN_ONE => 125 uV per bit
  return raw * 0.000125f;
}

float tempFromTMP(float vtmp) {
  // MCP9701-style conversion used by Rev P docs
  return (vtmp - 0.400f) / 0.0195f;
}

float windMpsFromRevP(float outVolts, float tempC, float zeroV) {
  // Avoid invalid pow for fractional exponent if temp <= 0
  if (tempC < 0.1f) tempC = 0.1f;

  float x = ((outVolts - zeroV) / (REG_A * pow(tempC, REG_B))) / REG_D;

  if (x <= 0.0f) return 0.0f;

  float windMph = pow(x, REG_C);
  return windMph * 0.44704f;
}

void printWindWarnings(float vout, float vtmp, float tempC) {
  if (vtmp < 0.30f || vtmp > 1.50f) {
    Serial.println("[WARN] TMP voltage out of expected range");
  }

  if (vout < 0.20f || vout > 2.30f) {
    Serial.println("[WARN] OUT voltage unusual for still/low wind");
  }

  if (tempC < -20.0f || tempC > 80.0f) {
    Serial.println("[WARN] Computed wind-sensor temperature looks implausible");
  }

  if (test_wind_zero < 1.20f || test_wind_zero > 1.50f) {
    Serial.println("[WARN] zeroWindVolts outside typical Rev P range");
  }
}

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
  Serial.println("[1] Wind Sensor (Rev P voltage model)");
  Serial.println("[2] Noise Sensor");
  Serial.println("[3] Light Sensor");
  Serial.println("[4] SCD41");
  Serial.println("[5] Globe Temp");
  Serial.println("[6] CO2 Calibration");
  Serial.println("[7] Exit + OTA");
  Serial.println("[0] Menu");
  Serial.println("---------------------------------------------");
  Serial.println("Wind commands:");
  Serial.println("  W1.346   -> set zero-wind voltage");
  Serial.println("  Z        -> start/stop auto-zero");
  Serial.println("  R        -> reset zero buffer");
  Serial.println("  N120     -> set auto-zero sample count");
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

  if (!ads.begin()) {
    Serial.println("[ERROR] ADS1115 not found. Check wiring.");
    while (true) {
      delay(1000);
    }
  }
  ads.setGain(GAIN_ONE);

  max31865.begin(MAX31865_4WIRE);

  Serial.print("[INFO] Default zeroWindVolts = ");
  Serial.println(test_wind_zero, 6);

  printMenu();
}

// ================= LOOP =================
void loop() {
  // ===== SERIAL INPUT =====
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();

    if (input.startsWith("W") || input.startsWith("w")) {
      float newZero = input.substring(1).toFloat();
      if (newZero > 0.0f && newZero < 5.0f) {
        test_wind_zero = newZero;
        Serial.print("[ZERO SET] zeroWindVolts = ");
        Serial.println(test_wind_zero, 6);
      } else {
        Serial.println("[ERROR] Invalid zero voltage");
      }

    } else if (input.equalsIgnoreCase("Z")) {
      autoZeroMode = !autoZeroMode;
      zeroAccumulator = 0.0f;
      zeroSamplesCollected = 0;

      if (autoZeroMode) {
        Serial.print("[AUTO ZERO STARTED] samples=");
        Serial.println(zeroSamplesTarget);
        Serial.println("Keep the sensor in still air.");
      } else {
        Serial.println("[AUTO ZERO STOPPED]");
      }

    } else if (input.equalsIgnoreCase("R")) {
      zeroAccumulator = 0.0f;
      zeroSamplesCollected = 0;
      Serial.println("[ZERO BUFFER RESET]");

    } else if (input.startsWith("N") || input.startsWith("n")) {
      int newCount = input.substring(1).toInt();
      if (newCount >= 10 && newCount <= 5000) {
        zeroSamplesTarget = newCount;
        Serial.print("[ZERO SAMPLE COUNT SET] ");
        Serial.println(zeroSamplesTarget);
      } else {
        Serial.println("[ERROR] Sample count must be 10..5000");
      }

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
        int16_t rawOut = ads.readADC_SingleEnded(0); // OUT
        int16_t rawTmp = ads.readADC_SingleEnded(1); // TMP

        float vOut = adcToVolts(rawOut);
        float vTmp = adcToVolts(rawTmp);
        float tempC = tempFromTMP(vTmp);
        float windMps = windMpsFromRevP(vOut, tempC, test_wind_zero);
        float windKmh = windMps * 3.6f;
        float windMph = windMps / 0.44704f;

        if (autoZeroMode) {
          zeroAccumulator += vOut;
          zeroSamplesCollected++;

          if (zeroSamplesCollected >= zeroSamplesTarget) {
            test_wind_zero = zeroAccumulator / (float)zeroSamplesCollected;
            Serial.println();
            Serial.print("[AUTO ZERO COMPLETE] zeroWindVolts = ");
            Serial.println(test_wind_zero, 6);

            autoZeroMode = false;
            zeroAccumulator = 0.0f;
            zeroSamplesCollected = 0;
          }
        }

        Serial.print("Wind | OUT=");
        Serial.print(vOut, 4);
        Serial.print(" V | TMP=");
        Serial.print(vTmp, 4);
        Serial.print(" V | Temp=");
        Serial.print(tempC, 2);
        Serial.print(" C | Zero=");
        Serial.print(test_wind_zero, 4);
        Serial.print(" V | Speed=");
        Serial.print(windMps, 4);
        Serial.print(" m/s | ");
        Serial.print(windKmh, 2);
        Serial.print(" km/h | ");
        Serial.print(windMph, 2);
        Serial.print(" mph");

        if (autoZeroMode) {
          Serial.print("  [ZERO ");
          Serial.print(zeroSamplesCollected);
          Serial.print("/");
          Serial.print(zeroSamplesTarget);
          Serial.print("]");
        }

        Serial.println();

        // Dataset/calibration line
        Serial.printf("CAL,%.4f,%.4f,%.2f,%.6f,%.4f\n",
                      vOut, vTmp, tempC, test_wind_zero, windMps);

        // Raw line
        Serial.printf("RAW,%.4f,%.4f\n", vOut, vTmp);

        printWindWarnings(vOut, vTmp, tempC);
        break;
      }

      // ================= NOISE =================
      case 2: {
        int16_t adc2 = ads.readADC_SingleEnded(2);
        float v = adcToVolts(adc2);
        float db = (v * 50.0f) + test_noise_offset;
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
        if (scd41.getDataReadyStatus() && scd41.readMeasurement()) {
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
