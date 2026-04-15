// ================= INCLUDES =================
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
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

// ================= MQTT =================
const char* mqtt_server = "7e43d9945d5748a782e8a2c3257f6743.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;
const char* mqtt_user = "ReACT";
const char* mqtt_pass = "ThermalComfort2026";

// ================= TOPICS =================
const char* topic_wind  = "sensor/wind_speed";
const char* topic_noise = "sensor/noise_db";
const char* topic_co2   = "sensor/co2";
const char* topic_temp  = "sensor/air_temperature";
const char* topic_hum   = "sensor/humidity";
const char* topic_globe = "sensor/globe_temperature";
const char* topic_lux   = "sensor/illuminance";

// ================= GLOBALS =================
WiFiClientSecure espClient;
PubSubClient client(espClient);

String deviceId;

// ===== Sensors =====
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

// ===== CALIBRATION VALUES =====
float wind_zero, wind_scale, wind_exp;
float noise_ref, noise_offset;
float lux_gain, lux_offset;
float globe_offset;
float scd_temp_offset, scd_hum_offset;

// ================= MQTT =================
void reconnectMQTT() {
  while (!client.connected()) {
    String clientId = "Station-" + deviceId;
    if (!client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      delay(3000);
    }
  }
}

// ================= FETCH CALIBRATION =================
void fetchCalibration() {

  HTTPClient http;
  http.begin(calibration_url);

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

// ================= SENSOR FUNCTIONS =================
float readWind() {
  int adc = ads.readADC_SingleEnded(0);
  float v = adc * 0.0000625;
  float x = (v - wind_zero) / wind_scale;
  return (x < 0) ? 0 : pow(x, wind_exp);
}

float readNoise() {
  noise.sample(samples, 100);
  NoiseData nd = processNoise(samples, 100);
  return 20 * log10((nd.vrms / noise_ref) + 1e-9) + noise_offset;
}

// ================= SETUP =================
void setup() {

  Serial.begin(115200);

  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.autoConnect("ESP32_SETUP");

  deviceId = WiFi.macAddress();

  espClient.setInsecure();
  client.setServer(mqtt_server, mqtt_port);
  ArduinoOTA.begin();

  Wire.begin(21, 22);

  scd.begin(Wire);
  light.begin();
  ads.begin();
  ads.setGain(GAIN_FOUR);
  globe.begin();

  fetchCalibration();
}

// ================= LOOP =================
void loop() {

  ArduinoOTA.handle();
  if (!client.connected()) reconnectMQTT();
  client.loop();

  static unsigned long last = 0;

  if (millis() - last > 2000) {
    last = millis();

    float wind = readWind();
    float noise = readNoise();

    SCD41Data d;
    scd.read(d);

    float temp = d.temp + scd_temp_offset;
    float hum  = d.hum + scd_hum_offset;

    float globeT = globe.readTemperature() + globe_offset;
    float lux = light.readLux() * lux_gain + lux_offset;

    char msg[16];

    dtostrf(wind,5,2,msg); client.publish(topic_wind,msg);
    dtostrf(noise,5,1,msg); client.publish(topic_noise,msg);
    dtostrf(d.co2,6,0,msg); client.publish(topic_co2,msg);
    dtostrf(temp,5,2,msg); client.publish(topic_temp,msg);
    dtostrf(hum,5,1,msg); client.publish(topic_hum,msg);
    dtostrf(globeT,5,2,msg); client.publish(topic_globe,msg);
    dtostrf(lux,6,0,msg); client.publish(topic_lux,msg);
  }
}}

float mockCO2() {
  return 400.0f + 200.0f * fabs(sin(t / 3.0f)) + random(-20, 20);
}

float mockTemp() {
  return 20.0f + 5.0f * sin(t / 5.0f);
}

float mockHumidity() {
  return 50.0f + 20.0f * sin(t / 6.0f);
}

float mockGlobeTemp(float airTemp) {
  return airTemp + 2.0f + 2.0f * sin(t / 4.0f);
}

float mockLux() {
  float dayCycle = fmaxf(0.0f, sin(t / 10.0f));
  return dayCycle * 800.0f + random(-20, 20);
}

#endif

// ===================== REAL SENSOR STUBS =====================
#if !SIMULATION_MODE

float readWind() { return 0; }
float readNoise() { return 0; }
float readCO2() { return 0; }
float readTemp() { return 0; }
float readHumidity() { return 0; }
float readGlobeTemp() { return 0; }
float readLux() { return 0; }

#endif

// ===================== SETUP =====================
void setup() {

  Serial.begin(115200);

  WiFi.mode(WIFI_STA);
  WiFiManager wm;

  if (!wm.autoConnect("ESP32_TEST_SETUP")) {
    ESP.restart();
  }

  Serial.println("WiFi connected");

  deviceId = WiFi.macAddress();
  Serial.println("Device ID: " + deviceId);

  espClient.setInsecure();
  client.setServer(mqtt_server, mqtt_port);

  ArduinoOTA.setHostname("ESP32-Test-Station");
  ArduinoOTA.begin();

#if SIMULATION_MODE
  Serial.println("Running in SIMULATION MODE");
#else
  Serial.println("Running with REAL SENSORS");
#endif

  Serial.println("System ready");
}

// ===================== LOOP =====================
void loop() {

  ArduinoOTA.handle();

  if (!client.connected()) {
    reconnectMQTT();
  }

  client.loop();

  static unsigned long lastSend = 0;

  if (millis() - lastSend > 2000) {
    lastSend = millis();

    float wind, noise, co2, temp, hum, globe, lux;

#if SIMULATION_MODE

    wind  = mockWind();
    noise = mockNoise();
    co2   = mockCO2();
    temp  = mockTemp();
    hum   = mockHumidity();
    globe = mockGlobeTemp(temp);
    lux   = mockLux();

#else

    wind  = readWind();
    noise = readNoise();
    co2   = readCO2();
    temp  = readTemp();
    hum   = readHumidity();
    globe = readGlobeTemp();
    lux   = readLux();

#endif

    // ===== SERIAL OUTPUT =====
    Serial.println("---- SENSOR DATA ----");
    Serial.printf("Wind: %.2f m/s\n", wind);
    Serial.printf("Noise: %.1f dB\n", noise);
    Serial.printf("CO2: %.0f ppm\n", co2);
    Serial.printf("Temp: %.2f C\n", temp);
    Serial.printf("Humidity: %.1f %%\n", hum);
    Serial.printf("Globe Temp: %.2f C\n", globe);
    Serial.printf("Lux: %.0f lx\n", lux);

    // ===== MQTT PUBLISH =====
    char msg[16];

    dtostrf(wind, 5, 2, msg);
    client.publish(topic_wind, msg);

    dtostrf(noise, 5, 1, msg);
    client.publish(topic_noise, msg);

    dtostrf(co2, 6, 0, msg);
    client.publish(topic_co2, msg);

    dtostrf(temp, 5, 2, msg);
    client.publish(topic_temp, msg);

    dtostrf(hum, 5, 1, msg);
    client.publish(topic_hum, msg);

    dtostrf(globe, 5, 2, msg);
    client.publish(topic_globe, msg);

    dtostrf(lux, 6, 0, msg);
    client.publish(topic_lux, msg);
  }
}
