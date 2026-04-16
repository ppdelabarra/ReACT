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
#include <PubSubClient.h> 

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
String command_topic;

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
float wind_temp_ref = 25.0;
float wind_temp_exp = 0.5;

float noise_offset = 0.0; 
float scd_temp_offset = 0.0;
float scd_hum_offset = 0.0;

// ================= OPT3001 CONFIG =================
void configureOPT3001() {
  OPT3001_Config cfg;
  cfg.RangeNumber = B1100;
  cfg.ConvertionTime = B0;
  cfg.Latch = B1;
  cfg.ModeOfConversionOperation = B11;
  opt3001.writeConfig(cfg);
}

// ================= MQTT CALLBACK =================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (int i = 0; i < length; i++) msg += (char)payload[i];

  if (String(topic) == command_topic && msg == "calibrate_co2") {
    scd41.stopPeriodicMeasurement();
    delay(500);
    scd41.performForcedRecalibration(400);
    delay(400);
    scd41.startPeriodicMeasurement();
  }
}

// ================= MQTT RECONNECT =================
void reconnectMQTT() {
  while (!mqttClient.connected()) {
    String clientId = "Station-" + deviceId;
    if (mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      mqttClient.publish((topic_base + "status").c_str(), "online", true);
      mqttClient.subscribe(command_topic.c_str());
    } else {
      delay(3000);
    }
  }
}

// ================= JSON CALIBRATION =================
void fetchCalibration() {
  WiFiClientSecure https;
  https.setInsecure();
  HTTPClient http;

  http.begin(https, calibration_url);
  if (http.GET() == 200) {
    StaticJsonDocument<4096> doc;
    if (!deserializeJson(doc, http.getString())) {
      if (doc["devices"].containsKey(deviceId)) {
        JsonObject dev = doc["devices"][deviceId];

        wind_zero  = dev["wind"]["zeroVolts"] | 0.0;
        wind_scale = dev["wind"]["scale"] | 1.0;
        wind_exp   = dev["wind"]["exponent"] | 1.0;
        wind_temp_ref = dev["wind"]["tempRef"] | 25.0;
        wind_temp_exp = dev["wind"]["tempExponent"] | 0.5;

        noise_offset = dev["noise"]["dbOffset"] | 0.0;
        scd_temp_offset = dev["scd41"]["tempOffset"] | 0.0;
        scd_hum_offset  = dev["scd41"]["humidityOffset"] | 0.0;
      }
    }
  }
  http.end();
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  preferences.begin("react_system", false);

  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.autoConnect("ReACT_Station_Setup");

  deviceId = WiFi.macAddress();
  topic_base = "sensor/" + deviceId + "/";
  command_topic = topic_base + "command";

  espClient.setInsecure();
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);

  fetchCalibration();

  Wire.begin(SDA_PIN, SCL_PIN);

  scd41.begin(Wire);
  scd41.startPeriodicMeasurement();

  opt3001.begin(OPT3001_ADDRESS);
  configureOPT3001();

  ads.begin();
  ads.setGain(GAIN_ONE);

  max31865.begin(MAX31865_4WIRE);
}

// ================= LOOP =================
void loop() {

  if (!mqttClient.connected()) reconnectMQTT();
  mqttClient.loop();

  if (millis() - lastSensorRead >= 5000) {
    lastSensorRead = millis();

    char msgBuffer[16];

    // --- SCD41 ---
    if (scd41.getDataReadyStatus() && scd41.readMeasurement()) {
      float temp = scd41.getTemperature() + scd_temp_offset;
      float hum = scd41.getHumidity() + scd_hum_offset;
      uint16_t co2 = scd41.getCO2();

      dtostrf(temp, 5, 2, msgBuffer); mqttClient.publish((topic_base + "temp").c_str(), msgBuffer);
      dtostrf(hum, 5, 1, msgBuffer);  mqttClient.publish((topic_base + "humidity").c_str(), msgBuffer);
      dtostrf(co2, 6, 0, msgBuffer);  mqttClient.publish((topic_base + "co2").c_str(), msgBuffer);
    }

    // --- LIGHT ---
    OPT3001 result = opt3001.readResult();
    dtostrf(result.lux, 6, 0, msgBuffer);
    mqttClient.publish((topic_base + "lux").c_str(), msgBuffer);

    // --- GLOBE ---
    float globeTemp = max31865.temperature(RNOMINAL, RREF);
    dtostrf(globeTemp, 5, 2, msgBuffer);
    mqttClient.publish((topic_base + "globe").c_str(), msgBuffer);

    // ================= WIND SENSOR =================
    int16_t adc0 = ads.readADC_SingleEnded(0);
    float voltage0 = adc0 * 0.000125;
    float simADC = (voltage0 / 5.0) * 1024.0;

    int16_t adc2 = ads.readADC_SingleEnded(2);
    float voltageTMP = adc2 * 0.000125;
    float tempTMP = (voltageTMP - 0.400) / 0.0195;

    float windSpeedMS = 0.0;

    if (wind_scale != 0.0) {
      float x = (simADC - wind_zero) / wind_scale;
      float windMPH = (x < 0) ? 0 : pow(x, wind_exp);

      float tempFactor = pow((tempTMP + 273.15) / (wind_temp_ref + 273.15), wind_temp_exp);
      windMPH *= tempFactor;

      windSpeedMS = windMPH * 0.44704;
    }

    dtostrf(windSpeedMS, 5, 2, msgBuffer);
    mqttClient.publish((topic_base + "wind").c_str(), msgBuffer);

    dtostrf(tempTMP, 5, 2, msgBuffer);
    mqttClient.publish((topic_base + "wind_temp").c_str(), msgBuffer);

    // --- NOISE ---
    int16_t adc1 = ads.readADC_SingleEnded(1);
    float voltage1 = adc1 * 0.000125;
    float noiseDb = (voltage1 * 50.0) + noise_offset;

    dtostrf(noiseDb, 5, 1, msgBuffer);
    mqttClient.publish((topic_base + "noise").c_str(), msgBuffer);
  }
}
