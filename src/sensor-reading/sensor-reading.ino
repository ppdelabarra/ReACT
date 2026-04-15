// ================= INCLUDES =================
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <math.h>
#include <Adafruit_ADS1X15.h>

#include "scd41_sensor.h"
#include "opt3001_sensor.h"
#include "noise_sensor.h"
#include "noise_processing.h"
#include "max31865_sensor.h"

// ================= CONFIG =================
#define SIMULATION_MODE false

const char* calibration_url =
"https://raw.githubusercontent.com/ppdelabarra/ReACT/main/calibration.json";

// ================= OTA =================
const char* currentVersion = "0.0";
const char* version_url =
"https://raw.githubusercontent.com/ppdelabarra/ReACT/main/version.json";

// ================= MQTT =================
const char* mqtt_server = "7e43d9945d5748a782e8a2c3257f6743.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;
const char* mqtt_user = "ReACT";
const char* mqtt_pass = "ThermalComfort2026";

// ================= GLOBALS =================
WiFiClientSecure espClient;
PubSubClient client(espClient);

String deviceId;
String topic_base;
String ota_topic;

unsigned long lastOTAcheck = 0;

// ================= SENSORS =================
SCD41Sensor scd;
OPT3001Sensor light;
NoiseSensor noise;
Adafruit_ADS1115 ads;

#define MAX_CS   5
#define MAX_MOSI 23
#define MAX_MISO 19
#define MAX_SCK  18
MAX31865Sensor globe(MAX_CS, MAX_MOSI, MAX_MISO, MAX_SCK);

float samples[100];

// ================= CALIBRATION =================
float wind_zero, wind_scale, wind_exp;
float noise_ref, noise_offset;
float lux_gain, lux_offset;
float globe_offset;
float scd_temp_offset, scd_hum_offset;

// ================= MQTT OTA CALLBACK =================
void mqttCallback(char* topic, byte* payload, unsigned int length) {

  String msg;

  for (int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  Serial.println("MQTT: " + msg);

  if (String(topic) == ota_topic) {
    if (msg == "update") {
      Serial.println("MQTT OTA trigger received");
      checkForUpdate();
    }
  }
}

// ================= MQTT RECONNECT =================
void reconnectMQTT() {

  while (!client.connected()) {

    String clientId = "Station-" + deviceId;

    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {

      Serial.println("MQTT connected");

      ota_topic = topic_base + "ota";
      client.subscribe(ota_topic.c_str());

    } else {
      delay(3000);
    }
  }
}

// ================= AUTO DISCOVERY =================
void publishDiscovery() {

  StaticJsonDocument<256> doc;

  doc["device"] = deviceId;
  doc["type"] = "ReACT_station";

  JsonArray sensors = doc.createNestedArray("sensors");
  sensors.add("wind");
  sensors.add("noise");
  sensors.add("temp");
  sensors.add("humidity");
  sensors.add("co2");
  sensors.add("globe");
  sensors.add("lux");

  char buffer[256];
  serializeJson(doc, buffer);

  client.publish("sensor/discovery", buffer, true);
}

// ================= CALIBRATION =================
void fetchCalibration() {

  HTTPClient http;
  WiFiClientSecure https;
  https.setInsecure();

  http.begin(https, calibration_url);
  // FIX: Allow redirect to GitHub CDN
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); 

  if (http.GET() == 200) {

    StaticJsonDocument<4096> doc;
    deserializeJson(doc, http.getString());

    JsonObject dev = doc["devices"][deviceId];

    wind_zero  = dev["wind"]["zeroVolts"];
    wind_scale = dev["wind"]["scale"];
    wind_exp   = dev["wind"]["exponent"];

    noise_ref    = dev["noise"]["refVrms"];
    noise_offset = dev["noise"]["dbOffset"];

    lux_gain   = dev["light"]["gain"];
    lux_offset = dev["light"]["offset"];

    globe_offset = dev["globe"]["offset"];

    scd_temp_offset = dev["scd41"]["tempOffset"];
    scd_hum_offset  = dev["scd41"]["humidityOffset"];

    Serial.println("Calibration loaded");
  }

  http.end();
}

// ================= OTA UPDATE =================
void updateFirmware(String url) {

  Serial.println("Starting OTA...");

  WiFiClientSecure updateClient;
  updateClient.setInsecure();
  
  // FIX: Allow redirect to GitHub CDN for the bin file
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  t_httpUpdate_return ret = httpUpdate.update(updateClient, url);

  if (ret == HTTP_UPDATE_OK) {
    Serial.println("OTA success → rebooting");
  } else if (ret == HTTP_UPDATE_NO_UPDATES) {
    Serial.println("No update");
  } else {
    Serial.printf("OTA failed: %d\n", httpUpdate.getLastError());
  }
}

// ================= VERSION CHECK =================
void checkForUpdate() {

  WiFiClientSecure https;
  https.setInsecure();

  HTTPClient http;
  http.begin(https, version_url);
  
  // FIX: Allow redirect to GitHub CDN for the json file
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (http.GET() == 200) {

    StaticJsonDocument<512> doc;
    deserializeJson(doc, http.getString());

    String latest = doc["version"];
    String bin = doc["bin"];

    Serial.println("Current: " + String(currentVersion));
    Serial.println("Latest: " + latest);

    if (latest != currentVersion) {

      Serial.println("New firmware found");

      client.disconnect();
      delay(1000);

      updateFirmware(bin);
    }
  }

  http.end();
}

// ================= WIND =================
float readWind() {

  int adc = ads.readADC_SingleEnded(0);
  float v = adc * 0.0000625;
  
  // FIX: Prevent divide-by-zero which causes "inf"
  if (wind_scale == 0.0) return -1.0; 
  
  float x = (v - wind_zero) / wind_scale;

  return (x < 0) ? 0 : pow(x, wind_exp);
}

// ================= NOISE =================
float readNoise() {

  noise.sample(samples, 100);
  NoiseData nd = processNoise(samples, 100);

  float vrms = nd.vrms;

  if (!isfinite(vrms)) return -1;
  if (vrms < 1e-6f) vrms = 1e-6f;
  if (noise_ref < 1e-6f) return -1;

  float ratio = vrms / noise_ref;
  if (ratio < 1e-6f) ratio = 1e-6f;

  return 20.0f * log10f(ratio) + noise_offset;
}

// ================= SETUP =================
void setup() {

  Serial.begin(115200);

  WiFi.mode(WIFI_STA);

  WiFiManager wm;
  wm.autoConnect("ESP32_SETUP");

  deviceId = WiFi.macAddress();
  Serial.println("MAC / Device ID: " + deviceId);
  topic_base = "sensor/" + deviceId + "/";

  espClient.setInsecure();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);

  ArduinoOTA.begin();

  Wire.begin(21, 22);

  scd.begin(Wire);
  light.begin();
  ads.begin();
  ads.setGain(GAIN_FOUR);
  globe.begin();

  fetchCalibration();
  reconnectMQTT();
  publishDiscovery();

  client.publish((topic_base + "status").c_str(), "online", true);

  Serial.println("System ready");
}

// ================= LOOP =================
void loop() {

  ArduinoOTA.handle();

  if (!client.connected()) reconnectMQTT();
  client.loop();

  static unsigned long last = 0;
  static unsigned long lastOTAcheck = 0;

  if (millis() - last > 2000) {

    last = millis();

    float wind = readWind();
    float noiseDb = readNoise();

    SCD41Data d;
    scd.read(d);

    float temp = d.temp + scd_temp_offset;
    float hum  = d.hum + scd_hum_offset;

    if (hum < 0) hum = 0;
    if (hum > 100) hum = 100;

    float globeT = globe.readTemperature() + globe_offset;
    float lux = light.readLux() * lux_gain + lux_offset;

    char msg[32];

    dtostrf(wind,5,2,msg); client.publish((topic_base + "wind").c_str(), msg);
    dtostrf(noiseDb,5,1,msg); client.publish((topic_base + "noise").c_str(), msg);
    dtostrf(d.co2,6,0,msg); client.publish((topic_base + "co2").c_str(), msg);
    dtostrf(temp,5,2,msg); client.publish((topic_base + "temp").c_str(), msg);
    dtostrf(hum,5,1,msg); client.publish((topic_base + "humidity").c_str(), msg);
    dtostrf(globeT,5,2,msg); client.publish((topic_base + "globe").c_str(), msg);
    dtostrf(lux,6,0,msg); client.publish((topic_base + "lux").c_str(), msg);

    // FEATURE: Display published values in Serial Monitor
    Serial.printf("MQTT Data -> Wind: %.2f | Noise: %.1f dB | CO2: %d ppm | Temp: %.2f C | Hum: %.1f %% | Globe: %.2f C | Lux: %.0f \n", 
                  wind, noiseDb, d.co2, temp, hum, globeT, lux);
  }

  // OTA periodic check (60s)
  if (millis() - lastOTAcheck > 60000) {
    lastOTAcheck = millis();
    // FEATURE: Log when checking for updates
    Serial.println("Running periodic OTA check..."); 
    checkForUpdate();
  }
}
