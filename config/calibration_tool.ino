// ================= INCLUDES =================
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <Adafruit_MAX31865.h>
#include <SparkFun_SCD4x_Arduino_Library.h>
#include <ClosedCube_OPT3001.h>

// Network & OTA Includes (Added for Option 7)
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

// ================= MENU STATE =================
int currentMode = 0; 
unsigned long lastReadTime = 0;
const unsigned long READ_INTERVAL = 2000; // Print data every 2 seconds

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
  Serial.println("Type a number and press Enter/Send:");
  Serial.println("  [1] - Wind Sensor (ADC0)");
  Serial.println("  [2] - Noise Sensor (ADC1)");
  Serial.println("  [3] - Light Sensor (OPT3001)");
  Serial.println("  [4] - SCD41 (Temp, Hum, CO2)");
  Serial.println("  [5] - Globe Temp (MAX31865)");
  Serial.println("---------------------------------------------");
  Serial.println("  [6] - EXECUTE CO2 CALIBRATION (400 ppm)");
  Serial.println("---------------------------------------------");
  Serial.println("  [7] - DOWNLOAD MAIN FIRMWARE & EXIT");
  Serial.println("---------------------------------------------");
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
    ads.setGain(GAIN_ONE);  // FIX: Allows reading up to +/- 4.096V
  }

  max31865.begin(MAX31865_4WIRE);
  Serial.println("MAX31865 initialized ✅");

  // Print the interactive menu once everything is loaded
  printMenu();
}

// ================= LOOP =================
void loop() {
  
  // 1. CHECK FOR USER INPUT
  if (Serial.available() > 0) {
    char incomingChar = Serial.read();
    
    // Ignore newline or carriage return characters
    if (incomingChar == '\n' || incomingChar == '\r') return;

    // Check if the input is a valid menu option (now 0 through 7)
    if (incomingChar >= '0' && incomingChar <= '7') {
      currentMode = incomingChar - '0'; 
      
      if (currentMode == 0) {
        printMenu();
        
      } else if (currentMode == 6) {
        // --- CO2 FORCED RECALIBRATION ---
        Serial.println("\n!!! INITIATING CO2 FORCED RECALIBRATION !!!");
        Serial.println("Ensure the sensor has been in fresh outside air for >3 minutes.");
        Serial.println("Stopping measurements...");
        
        scd41.stopPeriodicMeasurement();
        delay(500);
        
        Serial.println("Calibrating internal baseline to 400 ppm...");
        
        if (scd41.performForcedRecalibration(400)) {
          Serial.println("✅ SUCCESS: Baseline permanently set to 400 ppm!");
        } else {
          Serial.println("❌ FAILED: Could not calibrate. Check sensor connection.");
        }
        
        delay(400);
        scd41.startPeriodicMeasurement();
        
        Serial.println("Returning to main menu...\n");
        currentMode = 0;
        printMenu();

      } else if (currentMode == 7) {
        // --- OTA FIRMWARE SWAP ---
        Serial.println("\n!!! EXITING CALIBRATION MODE !!!");
        Serial.println("Preparing to download main firmware from GitHub...");
        
        // 1. Connect to Wi-Fi
        WiFi.mode(WIFI_STA);
        WiFiManager wm;
        Serial.println("Connecting to Wi-Fi...");
        if (!wm.autoConnect("ReACT_Station_Setup")) {
          Serial.println("❌ Wi-Fi Failed. Cannot download firmware.");
          currentMode = 0;
          printMenu();
          return; // Escape back to loop
        }
        
        Serial.println("✅ Wi-Fi Connected! Contacting GitHub...");
        
        // 2. Fetch the JSON to get the .bin URL
        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        
        http.begin(client, version_url);
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        
        if (http.GET() == 200) {
          StaticJsonDocument<512> doc;
          deserializeJson(doc, http.getString());
          String binUrl = doc["bin"];
          
          Serial.println("✅ Found firmware URL: " + binUrl);
          Serial.println("📥 Downloading and Flashing... DO NOT UNPLUG!");
          
          // 3. Download and apply the new firmware
          httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
          t_httpUpdate_return ret = httpUpdate.update(client, binUrl);
          
          if (ret == HTTP_UPDATE_OK) {
            Serial.println("✅ OTA Success! Rebooting into Main Firmware...");
            delay(1000);
            ESP.restart(); // The board will wake up running your main .ino!
          } else {
            Serial.printf("❌ OTA Failed Error (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
          }
        } else {
          Serial.println("❌ Failed to reach GitHub version.json.");
        }
        http.end();
        
        Serial.println("Returning to main menu...\n");
        currentMode = 0;
        printMenu();

      } else {
        // Normal sensor reading modes (1-5)
        Serial.printf("\n---> Switched to Mode [%d] <---\n", currentMode);
        lastReadTime = millis() - READ_INTERVAL; 
      }
    } else {
      Serial.println("Invalid input. Please type a number from 0 to 7.");
    }
  }

  // 2. READ SENSORS BASED ON CURRENT MODE (1 through 5)
  if (currentMode >= 1 && currentMode <= 5 && (millis() - lastReadTime >= READ_INTERVAL)) {
    lastReadTime = millis();

    switch (currentMode) {
      
      case 1: { // WIND
        int16_t adc0 = ads.readADC_SingleEnded(0);
        float voltage0 = adc0 * 0.000125; 
        float simulatedArduinoADC = (voltage0 / 5.0) * 1024.0;
        Serial.printf("Wind -> Raw Volts: %.3f V | Sim ADC: %.1f\n", voltage0, simulatedArduinoADC);
        break;
      }
      
      case 2: { // NOISE
        int16_t adc1 = ads.readADC_SingleEnded(1);
        float voltage1 = adc1 * 0.000125; 
        if (voltage1 < 0) voltage1 = 0;
        float rawDBA = voltage1 * 50.0;
        Serial.printf("Noise -> Raw Volts: %.3f V | Calc dBA: %.1f\n", voltage1, rawDBA);
        break;
      }
      
      case 3: { // LIGHT
        OPT3001 result = opt3001.readResult();
        if (result.error == NO_ERROR) {
          Serial.printf("Light -> Raw Lux: %.2f\n", result.lux);
        } else {
          Serial.println("Light -> Read Error");
        }
        break;
      }
      
      case 4: { // SCD41
        if (scd41.getDataReadyStatus() && scd41.readMeasurement()) {
          Serial.printf("SCD41 -> Raw Temp: %.2f °C | Raw Hum: %.1f %% | Raw CO2: %d ppm\n", 
                        scd41.getTemperature(), scd41.getHumidity(), scd41.getCO2());
        } else {
          Serial.println("SCD41 -> Not ready");
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
