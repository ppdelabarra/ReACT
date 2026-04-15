// ================= INCLUDES =================
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <Adafruit_MAX31865.h>
#include <SparkFun_SCD4x_Arduino_Library.h>
#include <ClosedCube_OPT3001.h>
#include <math.h> // Required for wind math

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

// Live Test Variables
float test_wind_zero = 264.0;
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

void printMenu() {
  Serial.println("\n=============================================");
  Serial.println("         SENSOR CALIBRATION MENU");
  Serial.println("=============================================");
  Serial.println(" Make sure Serial Monitor is set to 'Newline'");
  Serial.println("---------------------------------------------");
  Serial.println("  [1] - Wind Sensor (Raw + Live Test)");
  Serial.println("  [2] - Noise Sensor (Raw + Live Test)");
  Serial.println("  [3] - Light Sensor (OPT3001)");
  Serial.println("  [4] - SCD41 (Temp, Hum, CO2)");
  Serial.println("  [5] - Globe Temp (MAX31865)");
  Serial.println("---------------------------------------------");
  Serial.println("  [6] - EXECUTE CO2 CALIBRATION (400 ppm)");
  Serial.println("  [7] - DOWNLOAD MAIN FIRMWARE & EXIT");
  Serial.println("---------------------------------------------");
  Serial.println(" LIVE TEST COMMANDS:");
  Serial.println("  Type W<number> to test Wind Zero (e.g. W267.3)");
  Serial.println("  Type N<number> to test Noise Offset (e.g. N-2.5)");
  Serial.println("  [0] - Stop reading and show menu");
  Serial.println("=============================================\n");
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
    ads.setGain(GAIN_ONE); 
  }

  max31865.begin(MAX31865_4WIRE);
  Serial.println("MAX31865 initialized ✅");

  printMenu();
}

// ================= LOOP =================
void loop() {
  
  // 1. CHECK FOR USER INPUT (ADVANCED PARSER)
  if (Serial.available() > 0) {
    String input = Serial.readStringUntil('\n');
    input.trim(); // Remove invisible whitespace
    
    if (input.length() == 0) return;

    // A. Check for Wind Test Command
    if (input.startsWith("W") || input.startsWith("w")) {
      test_wind_zero = input.substring(1).toFloat();
      Serial.printf("\n✅ TEST APPLIED: Wind Zero set to %.2f\n", test_wind_zero);
      return;
    }

    // B. Check for Noise Test Command
    if (input.startsWith("N") || input.startsWith("n")) {
      test_noise_offset = input.substring(1).toFloat();
      Serial.printf("\n✅ TEST APPLIED: Noise Offset set to %.2f dB\n", test_noise_offset);
      return;
    }

    // C. Check for Standard Menu Commands (0-7)
    if (input.length() == 1 && input[0] >= '0' && input[0] <= '7') {
      currentMode = input[0] - '0'; 
      
      if (currentMode == 0) {
        printMenu();
        
      } else if (currentMode == 6) {
        // --- CO2 FORCED RECALIBRATION ---
        Serial.println("\n!!! INITIATING CO2 FORCED RECALIBRATION !!!");
        scd41.stopPeriodicMeasurement();
        delay(500);
        if (scd41.performForcedRecalibration(400)) {
          Serial.println("✅ SUCCESS: Baseline permanently set to 400 ppm!");
        } else {
          Serial.println("❌ FAILED: Could not calibrate.");
        }
        delay(400);
        scd41.startPeriodicMeasurement();
        currentMode = 0;
        printMenu();

      } else if (currentMode == 7) {
        // --- OTA FIRMWARE SWAP ---
        Serial.println("\n!!! EXITING CALIBRATION MODE !!!");
        WiFi.mode(WIFI_STA);
        WiFiManager wm;
        if (!wm.autoConnect("ReACT_Station_Setup")) {
          Serial.println("❌ Wi-Fi Failed.");
          currentMode = 0;
          return;
        }
        
        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        http.begin(client, version_url);
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        
        if (http.GET() == 200) {
          StaticJsonDocument<512> doc;
          deserializeJson(doc, http.getString());
          String binUrl = doc["bin"];
          
          Serial.println("📥 Downloading Main Firmware... DO NOT UNPLUG!");
          httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
          if (httpUpdate.update(client, binUrl) == HTTP_UPDATE_OK) {
            Serial.println("✅ OTA Success! Rebooting...");
            delay(1000);
            ESP.restart(); 
          } else {
            Serial.printf("❌ OTA Failed Error (%d)\n", httpUpdate.getLastError());
          }
        }
        http.end();
        currentMode = 0;
        printMenu();

      } else {
        Serial.printf("\n---> Switched to Mode [%d] <---\n", currentMode);
        lastReadTime = millis() - READ_INTERVAL; 
      }
    } else {
      Serial.println("Invalid input. Type a number (0-7) or a test command (W/N).");
    }
  }

  // 2. READ SENSORS BASED ON CURRENT MODE
  if (currentMode >= 1 && currentMode <= 5 && (millis() - lastReadTime >= READ_INTERVAL)) {
    lastReadTime = millis();

    switch (currentMode) {
      
      case 1: { // WIND (With Live Math)
        int16_t adc0 = ads.readADC_SingleEnded(0);
        float voltage0 = adc0 * 0.000125; 
        float simulatedArduinoADC = (voltage0 / 5.0) * 1024.0;
        
        // Calculate Live Test Speed
        float windSpeedMS = 0.0;
        float x = (simulatedArduinoADC - test_wind_zero) / 85.6814;
        float windMPH = (x < 0) ? 0 : pow(x, 3.36814);
        windSpeedMS = windMPH * 0.44704;

        Serial.printf("Wind -> Raw: %.3f V | Sim ADC: %.1f || LIVE TEST (Zero=%.1f): %.2f m/s\n", 
                      voltage0, simulatedArduinoADC, test_wind_zero, windSpeedMS);
        break;
      }
      
      case 2: { // NOISE (With Live Math)
        int16_t adc1 = ads.readADC_SingleEnded(1);
        float voltage1 = adc1 * 0.000125; 
        if (voltage1 < 0) voltage1 = 0;
        float rawDBA = voltage1 * 50.0;
        float testDBA = rawDBA + test_noise_offset;

        Serial.printf("Noise -> Raw: %.3f V | Raw SPL: %.1f || LIVE TEST (Offset=%.1f): %.1f dBA\n", 
                      voltage1, rawDBA, test_noise_offset, testDBA);
        break;
      }
      
      case 3: { // LIGHT
        OPT3001 result = opt3001.readResult();
        if (result.error == NO_ERROR) Serial.printf("Light -> Raw Lux: %.2f\n", result.lux);
        break;
      }
      
      case 4: { // SCD41
        if (scd41.getDataReadyStatus() && scd41.readMeasurement()) {
          Serial.printf("SCD41 -> Raw Temp: %.2f °C | Raw Hum: %.1f %% | Raw CO2: %d ppm\n", 
                        scd41.getTemperature(), scd41.getHumidity(), scd41.getCO2());
        }
        break;
      }
      
      case 5: { // GLOBE TEMP
        float globeTemp = max31865.temperature(RNOMINAL, RREF);
        Serial.printf("Globe -> Raw Temp: %.2f °C\n", globeTemp);
        break;
      }
    }
  }
}
