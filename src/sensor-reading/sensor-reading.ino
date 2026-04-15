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
#include <Preferences.h> 
#include <PubSubClient.h> // <-- MQTT IS BACK

// ================= GITHUB / OTA CONFIG =================
const char* version_url = "https://raw.githubusercontent.com/ppdelabarra/ReACT/main/version.json";
const char* calibration_url = "https://raw.githubusercontent.com/ppdelabarra/ReACT/main/calibration.json";

Preferences preferences;
String currentVersion;
String deviceId;

unsigned long lastSensorRead = 0;
unsigned long lastOTAcheck = 0;

// ================= MQTT CONFIG =================
const char* mqtt_server = "7e43d9945d5748a782e8a2c3257f6743.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;
const char* mqtt_user = "ReACT";
const char* mqtt_pass = "ThermalComfort2026";

WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);
String topic_base;

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

// ================= MQTT RECONNECT =================
void reconnectMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("Connecting to MQTT...");
    String clientId = "Station-" + deviceId;
    
    if (mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      Serial.println(" Connected! ✅");
      mqttClient.publish((topic_base + "status").c_str(), "online", true);
    } else {
      Serial.print(" Failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" Trying again in 3 seconds...");
      delay(3000);
    }
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
void updateFirmware(String url, String newVersion) {
  Serial.println("Starting OTA Download from: " + url);
  WiFiClientSecure updateClient;
  updateClient.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.rebootOnUpdate(false);

  t_httpUpdate_return ret = httpUpdate.update(updateClient, url);

  if (ret == HTTP_UPDATE_OK) {
    preferences.putString("fw_version", newVersion);
    Serial.println("OTA success! Version officially updated to " + newVersion + " ✅");
    delay(1000);
    ESP.restart(); 
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

      if (latest != currentVersion) {
        Serial.println("Versions do not match. Initiating update...");
        delay(1000);
        updateFirmware(binUrl, latest);
      } else {
        Serial.println("Firmware is up to date.");
      }
    }
  }
  http.end();
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n=== SYSTEM BOOT ===");

  // --- Initialize Memory & Load Version ---
  preferences.begin("react_system", false); 
  currentVersion = preferences.getString("fw_version", "0.0"); 
  Serial.println("Running Firmware Version: " + currentVersion);

  // --- Wi-Fi Setup ---
  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  Serial.println("Connecting to Wi-Fi...");
  if(wm.autoConnect("ReACT_Station_Setup")) {
    Serial.println("Wi-Fi Connected ✅");
  } else {
    Serial.println("Wi-Fi Connection Failed ❌");
  }

  deviceId = WiFi.macAddress();
  Serial.println("Device MAC ID: " + deviceId);
  topic_base = "sensor/" + deviceId + "/";

  // --- MQTT Setup ---
  espClient.setInsecure(); // Required for HiveMQ TLS
  mqttClient.setServer(mqtt_server, mqtt_port);

  // --- Fetch Cloud Configurations ---
  fetchCalibration();
  checkForUpdate();

  Serial.println("\n=== SENSOR INITIALIZATION ===");
  Wire.begin(SDA_PIN, SCL_PIN);

  if (scd41.begin(Wire)) {
    Serial.println("SCD41 detected ✅");
    scd41.startPeriodicMeasurement();
  }
  
  opt3001.begin(OPT3001_ADDRESS);
  configureOPT3001();

  if (ads.begin()) {
    Serial.println("ADS1115 detected ✅");
    ads.setGain(GAIN_FOUR);
  }

  max31865.begin(MAX31865_4WIRE);
  Serial.println("MAX31865 initialized ✅");
  Serial.println("================================\n");
}

// ================= LOOP =================
void loop() {
  // Keep MQTT Alive
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }
  mqttClient.loop();

  unsigned long currentMillis = millis();

  // --- Read Sensors Every 5 Seconds ---
  if (currentMillis - lastSensorRead >= 5000) {
    lastSensorRead = currentMillis;
    
    Serial.println("\n--- SENSOR READINGS ---");
    char msgBuffer[16]; // Buffer for formatting numbers to strings

    // SCD41 (CO2, Temp, Hum)
    if (scd41.getDataReadyStatus() && scd41.readMeasurement()) {
      float temp = scd41.getTemperature();
      float hum = scd41.getHumidity();
      uint16_t co2 = scd41.getCO2();
      
      Serial.printf("SCD41 -> Temp: %.2f °C | Hum: %.1f %% | CO2: %d ppm\n", temp, hum, co2);
      
      dtostrf(temp, 5, 2, msgBuffer); mqttClient.publish((topic_base + "temp").c_str(), msgBuffer);
      dtostrf(hum, 5, 1, msgBuffer);  mqttClient.publish((topic_base + "humidity").c_str(), msgBuffer);
      dtostrf(co2, 6, 0, msgBuffer);  mqttClient.publish((topic_base + "co2").c_str(), msgBuffer);
    }

    // OPT3001 (Light)
    OPT3001 result = opt3001.readResult();
    if (result.error == NO_ERROR) {
      Serial.printf("OPT3001 -> Illuminance: %.2f lux\n", result.lux);
      dtostrf(result.lux, 6, 0, msgBuffer); mqttClient.publish((topic_base + "lux").c_str(), msgBuffer);
    }

    // MAX31865 (Globe Temp)
    float globeTemp = max31865.temperature(RNOMINAL, RREF);
    uint8_t fault = max31865.readFault();
    Serial.printf("MAX31865 -> Globe Temp: %.2f °C\n", globeTemp);
    if (!fault) {
      dtostrf(globeTemp, 5, 2, msgBuffer); mqttClient.publish((topic_base + "globe").c_str(), msgBuffer);
    } else {
      max31865.clearFault();
    }

    // Wind Processing (ADC0)
    int16_t adc0 = ads.readADC_SingleEnded(0);
    float voltage0 = adc0 * 0.0000625;
    float windSpeed = 0;
    
    if (wind_scale != 0.0) {
      float x = (voltage0 - wind_zero) / wind_scale;
      windSpeed = (x < 0) ? 0 : pow(x, wind_exp);
    }
    Serial.printf("WIND -> Raw: %.3f V | Speed: %.2f m/s\n", voltage0, windSpeed);
    dtostrf(windSpeed, 5, 2, msgBuffer); mqttClient.publish((topic_base + "wind").c_str(), msgBuffer);

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
    dtostrf(noiseDb, 5, 1, msgBuffer); mqttClient.publish((topic_base + "noise").c_str(), msgBuffer);
    
    Serial.println("--- Data published to MQTT ---");
  }

  // --- Check for OTA Every 60 Seconds ---
  if (currentMillis - lastOTAcheck >= 60000) {
    lastOTAcheck = currentMillis;
    checkForUpdate();
  }
}
