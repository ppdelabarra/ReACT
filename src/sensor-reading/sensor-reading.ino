#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>

// ===================== OTA VERSION =====================
const char* currentVersion = "0.0";
const char* version_url = "https://raw.githubusercontent.com/ppdelabarra/ReACT/main/version.json";

// ===================== CALIBRATION =====================
const char* calibration_url =
"https://raw.githubusercontent.com/ppdelabarra/ReACT/main/config/calibration.json";

// ===================== MQTT =====================
const char* mqtt_server = "7e43d9945d5748a782e8a2c3257f6743.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;
const char* mqtt_user = "ReACT";
const char* mqtt_pass = "ThermalComfort2026";

const char* topic_wind = "sensor/wind_speed";

// ===================== ADS1115 =====================
Adafruit_ADS1115 ads;

const float ADS_VREF = 4.096;
const float ADS_COUNTS = 32767.0;

// ===================== MQTT CLIENT =====================
WiFiClientSecure espClient;
PubSubClient client(espClient);

// ===================== DEVICE ID =====================
String deviceId;

// ===================== CALIBRATION STRUCT =====================
struct Calibration {
  float zeroVolts;
  float scale;
  float exponent;
};

Calibration calib;

// ===================== OTA UPDATE =====================
void updateFirmware(String url) {
  Serial.println("OTA update starting...");
  WiFiClientSecure updateClient;
  updateClient.setInsecure();

  t_httpUpdate_return ret = httpUpdate.update(updateClient, url);

  if (ret == HTTP_UPDATE_OK) {
    Serial.println("Update OK → rebooting");
  } else if (ret == HTTP_UPDATE_NO_UPDATES) {
    Serial.println("No update");
  } else {
    Serial.printf("Update failed: %d\n", httpUpdate.getLastError());
  }
}

// ===================== VERSION CHECK =====================
void checkForUpdate() {

  WiFiClientSecure https;
  https.setInsecure();

  HTTPClient http;
  http.begin(https, version_url);

  int code = http.GET();

  if (code == 200) {

    String payload = http.getString();

    StaticJsonDocument<512> doc;
    deserializeJson(doc, payload);

    String latest = doc["version"];
    String bin = doc["bin"];

    if (latest != currentVersion) {
      Serial.println("New firmware found");
      client.disconnect();
      delay(1000);
      updateFirmware(bin);
    } else {
      Serial.println("Firmware up to date");
    }
  }

  http.end();
}

// ===================== LOAD CALIBRATION =====================
void loadCalibration() {

  WiFiClientSecure clientSecure;
  clientSecure.setInsecure();

  HTTPClient http;
  http.begin(clientSecure, calibration_url);

  int code = http.GET();

  if (code == 200) {

    String payload = http.getString();

    StaticJsonDocument<2048> doc;
    deserializeJson(doc, payload);

    JsonObject devices = doc["devices"];
    JsonObject dev = devices[deviceId];

    if (!dev.isNull()) {

      calib.zeroVolts = dev["zeroVolts"];
      calib.scale = dev["scale"];
      calib.exponent = dev["exponent"];

      Serial.println("Calibration loaded ✔");

    } else {

      Serial.println("No calibration found → using defaults");

      calib.zeroVolts = 1.075;
      calib.scale = 0.230;
      calib.exponent = 2.7265;
    }
  }

  http.end();
}

// ===================== MQTT RECONNECT =====================
void reconnectMQTT() {

  while (!client.connected()) {

    String clientId = "WindSensor-" + deviceId;

    Serial.println("Connecting MQTT...");

    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      Serial.println("MQTT connected");
    } else {
      Serial.printf("MQTT failed rc=%d\n", client.state());
      delay(3000);
    }
  }
}

// ===================== WIND SENSOR =====================
float readWindMS() {

  int16_t raw = ads.readADC_SingleEnded(0);

  float volts = raw * (ADS_VREF / ADS_COUNTS);

  if (volts <= calib.zeroVolts) return 0.0;

  float windMPH = pow(
    ((volts - calib.zeroVolts) / calib.scale),
    calib.exponent
  );

  return windMPH * 0.44704;
}

// ===================== SETUP =====================
void setup() {

  Serial.begin(115200);

  WiFi.mode(WIFI_STA);

  WiFiManager wm;
  if (!wm.autoConnect("ESP32_Wind_Setup")) {
    ESP.restart();
  }

  Serial.println("WiFi connected");

  deviceId = WiFi.macAddress();
  Serial.println("Device ID: " + deviceId);

  // MQTT secure (dev mode insecure TLS)
  espClient.setInsecure();
  client.setServer(mqtt_server, mqtt_port);

  // OTA
  ArduinoOTA.setHostname("ESP32-Wind-Sensor");
  ArduinoOTA.begin();

  // ADS1115 init
  Wire.begin();
  if (!ads.begin()) {
    Serial.println("ADS1115 not found!");
    while (1);
  }

  ads.setGain(GAIN_ONE);

  // Load calibration from GitHub
  loadCalibration();

  // Check firmware
  checkForUpdate();

  Serial.println("System ready");
}

// ===================== LOOP =====================
void loop() {

  ArduinoOTA.handle();

  if (!client.connected()) {
    reconnectMQTT();
  }

  client.loop();

  static unsigned long lastSensor = 0;
  static unsigned long lastUpdate = 0;

  // ===== SENSOR =====
  if (millis() - lastSensor > 2000) {
    lastSensor = millis();

    float windMS = readWindMS();

    char msg[16];
    dtostrf(windMS, 5, 2, msg);

    client.publish(topic_wind, msg);

    Serial.printf("Wind: %.2f m/s\n", windMS);
  }

  // ===== OTA CHECK (60s) =====
  if (millis() - lastUpdate > 60000) {
    lastUpdate = millis();
    checkForUpdate();
  }
}
