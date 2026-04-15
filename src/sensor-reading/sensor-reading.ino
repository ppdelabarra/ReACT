// ================= INCLUDES =================
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <Adafruit_MAX31865.h>
#include <SparkFun_SCD4x_Arduino_Library.h>
#include <ClosedCube_OPT3001.h>
#include <math.h>

// Network, OTA & Memory Includes
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include <Preferences.h> // <-- NEW: Allows us to save data permanently

// ================= GITHUB / OTA CONFIG =================
const char* version_url = "https://raw.githubusercontent.com/ppdelabarra/ReACT/main/version.json";
const char* calibration_url = "https://raw.githubusercontent.com/ppdelabarra/ReACT/main/calibration.json";

Preferences preferences;
String currentVersion;
String deviceId;

unsigned long lastSensorRead = 0;
unsigned long lastOTAcheck = 0;

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

// ================= CALIBRATION GLOBALS =================
float wind_zero = 0.0, wind_scale = 1.0, wind_exp = 1.0;
float noise_ref = 0.01, noise_offset = 60.0;

// ================= OPT3001 CONFIG =================
void configureOPT3001() {
  OPT3001_Config cfg;
  cfg.RangeNumber = B1100;
  cfg.ConvertionTime = B0;
  cfg.Latch = B1;
  cfg.ModeOfConversionOperation = B11;

  OPT3001_ErrorCode err = opt3001.writeConfig(cfg);
  if (err == NO_ERROR) {
    Serial.println("OPT3001 configured ✅");
  } else {
    Serial.printf("OPT3001 config error: %d\n", err);
  }
}

// ================= JSON CALIBRATION =================
void fetchCalibration() {
  Serial.println("Fetching calibration from GitHub...");
  
  WiFiClientSecure https;
  https.setInsecure();
  HTTPClient http;
  
  http.begin(https, calibration_url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int httpCode = http.GET();
  if (httpCode == 200) {
    StaticJsonDocument<4096> doc;
    DeserializationError error = deserializeJson(doc, http.getString());
    
    if (!error) {
      if (doc["devices"].containsKey(deviceId)) {
        JsonObject dev = doc["devices"][deviceId];

        wind_zero  = dev["wind"]["zeroVolts"] | 0.0;
        wind_scale = dev["wind"]["scale"] | 1.0;
        wind_exp   = dev["wind"]["exponent"] | 1.0;

        noise_ref    = dev["noise"]["refVrms"] | 0.01;
        noise_offset = dev["noise"]["dbOffset"] | 60.0;

        Serial.println("Calibration loaded successfully ✅");
      } else {
        Serial.println("Device MAC not found in JSON. Using defaults ⚠️");
      }
    } else {
      Serial.println("Failed to parse JSON ❌");
    }
  } else {
    Serial.printf("Failed to download calibration file ❌ HTTP Code: %d\n", httpCode);
  }
  http.end();
}

// ================= OTA FIRMWARE UPDATE =================
// Notice we now pass the newVersion string into this function
void updateFirmware(String url, String newVersion) {
  Serial.println("Starting OTA Download from: " + url);

  WiFiClientSecure updateClient;
  updateClient.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  
  // Disable automatic reboot so we can write to memory first
  httpUpdate.rebootOnUpdate(false);

  t_httpUpdate_return ret = httpUpdate.update(updateClient, url);

  if (ret == HTTP_UPDATE_OK) {
    // --- THIS IS THE MAGIC ---
    // Update was successful, so we permanently save the new version to memory
    preferences.putString("fw_version", newVersion);
    
    Serial.println("OTA success! Version officially updated to " + newVersion + " ✅");
    Serial.println("Rebooting in 1 second...");
    delay(1000);
    ESP.restart(); // Now we manually reboot
  } else if (ret == HTTP_UPDATE_NO_UPDATES) {
    Serial.println("No update found.");
  } else {
    Serial.printf("OTA failed: %d ❌\n", httpUpdate.getLastError());
  }
}

void checkForUpdate() {
  Serial.println("Checking for firmware updates...");
  
  WiFiClientSecure https;
  https.setInsecure();
  HTTPClient http;
  
  http.begin(https, version_url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (http.GET() == 200) {
    StaticJsonDocument<512> doc;
    if (!deserializeJson(doc, http.getString())) {
      String latest = doc["version"];
      String binUrl = doc["bin"];

      Serial.println("Device Version: " + currentVersion);
      Serial.println("GitHub Version: " + latest);

      // Compare the string saved in memory to the one on GitHub
      if (latest != currentVersion) {
        Serial.println("Versions do not match. Initiating update...");
        delay(1000);
        updateFirmware(binUrl, latest); // Pass the URL and the target version
      } else {
        Serial.println("Firmware is up to date.");
      }
    }
  } else {
    Serial.println("Failed to check for updates ❌");
  }
  http.end();
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n=== SYSTEM BOOT ===");

  // --- Initialize Memory & Load Version ---
  preferences.begin("react_system", false); // Open memory space named "react_system"
  // Grab "fw_version". If it doesn't exist yet, default to "0.0"
  currentVersion = preferences.getString("fw_version", "0.0"); 
  
  Serial.println("Running Firmware Version: " + currentVersion);

  // --- Wi-Fi Setup ---
  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  Serial.println("Connecting to Wi-Fi...");
  bool connected = wm.autoConnect("ReACT_Station_Setup");
  
  if(connected) {
    Serial.println("Wi-Fi Connected ✅");
  } else {
    Serial.println("Wi-Fi Connection Failed ❌");
  }

  deviceId = WiFi.macAddress();
  Serial.println("Device MAC ID: " + deviceId);

  // --- Fetch Cloud Configurations ---
  fetchCalibration();
  checkForUpdate();

  Serial.println("\n=== SENSOR INITIALIZATION ===");

  Wire.begin(SDA_PIN, SCL_PIN);

  // --- SCD41 ---
  if (scd41.begin(Wire)) {
    Serial.println("SCD41 detected ✅");
    scd41.startPeriodicMeasurement();
  } else {
    Serial.println("SCD41 NOT detected ❌");
  }

  // --- OPT3001 ---
  opt3001.begin(OPT3001_ADDRESS);
  configureOPT3001();

  // --- ADS1115 ---
  if (ads.begin()) {
    Serial.println("ADS1115 detected ✅");
    ads.setGain(GAIN_FOUR);  // ±1.024V range
  } else {
    Serial.println("ADS1115 NOT detected ❌");
  }

  // --- MAX31865 ---
  max31865.begin(MAX31865_4WIRE);
  Serial.println("MAX31865 initialized ✅");

  Serial.println("================================\n");
}

// ================= LOOP =================
void loop() {
  unsigned long currentMillis = millis();

  // --- Read Sensors Every 5 Seconds ---
  if (currentMillis - lastSensorRead >= 5000) {
    lastSensorRead = currentMillis;
    
    Serial.println("\n--- SENSOR READINGS ---");

    // SCD41 (CO2, Temp, Hum)
    if (scd41.getDataReadyStatus() && scd41.readMeasurement()) {
      Serial.printf("SCD41 -> Temp: %.2f °C | Hum: %.1f %% | CO2: %d ppm\n", 
                    scd41.getTemperature(), scd41.getHumidity(), scd41.getCO2());
    } else {
      Serial.println("SCD41 -> Not ready or failed");
    }

    // OPT3001 (Light)
    OPT3001 result = opt3001.readResult();
    if (result.error == NO_ERROR) {
      Serial.printf("OPT3001 -> Illuminance: %.2f lux\n", result.lux);
    }

    // MAX31865 (Globe Temp)
    float globeTemp = max31865.temperature(RNOMINAL, RREF);
    uint8_t fault = max31865.readFault();
    Serial.printf("MAX31865 -> Globe Temp: %.2f °C\n", globeTemp);
    if (fault) {
      Serial.printf("MAX31865 -> Fault: 0x%X\n", fault);
      max31865.clearFault();
    }

    // Wind Processing (ADC0)
    int16_t adc0 = ads.readADC_SingleEnded(0);
    float voltage0 = adc0 * 0.0000625;
    float windSpeed = 0;
    
    if (wind_scale != 0.0) { // Protect against divide-by-zero
      float x = (voltage0 - wind_zero) / wind_scale;
      windSpeed = (x < 0) ? 0 : pow(x, wind_exp);
    }
    Serial.printf("WIND -> Raw: %.3f V | Speed: %.2f m/s\n", voltage0, windSpeed);

    // Noise Processing (ADC1)
    const int N_SAMPLES = 100;
    float sum = 0, sumSq = 0;

    for (int i = 0; i < N_SAMPLES; i++) {
      float v = ads.readADC_SingleEnded(1) * 0.0000625;
      sum += v;
      sumSq += v * v;
      delayMicroseconds(200);
    }

    float mean = sum / N_SAMPLES;
    float variance = (sumSq / N_SAMPLES) - (mean * mean);
    if (variance < 0) variance = 0;
    float vrms = sqrt(variance);

    float noiseDb = -1.0;
    if (isfinite(vrms) && noise_ref >= 1e-6f) {
      if (vrms < 1e-6f) vrms = 1e-6f;
      float ratio = vrms / noise_ref;
      if (ratio < 1e-6f) ratio = 1e-6f;
      noiseDb = 20.0f * log10(ratio) + noise_offset;
    }
    
    Serial.printf("NOISE -> RMS: %.3f mV | SPL: %.1f dB\n", (vrms * 1000.0), noiseDb);
    Serial.println("------------------------");
  }

  // --- Check for OTA Every 60 Seconds ---
  if (currentMillis - lastOTAcheck >= 60000) {
    lastOTAcheck = currentMillis;
    checkForUpdate();
  }
}
