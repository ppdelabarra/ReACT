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
// Rev P wind sensor: only zero-wind voltage is needed for the new regression
float wind_zero = 1.346f;

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
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

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

// ================= REV P WIND REGRESSION =================
// outVolts: OUT voltage from Rev P
// tempC: TMP-derived temperature in Celsius
// zeroWindVolts: calibrated zero-wind OUT voltage
float computeWindMS_RevP(float outVolts, float tempC, float zeroWindVolts) {
  // Modern Device Rev P regression constants
  const float A = 3.038517f;
  const float B = 0.115157f;
  const float C = 3.009364f;
  const float D = 0.087288f;

  // Prevent invalid fractional power for very low/negative temperatures
  if (tempC < 0.1f) tempC = 0.1f;

  float x = ((outVolts - zeroWindVolts) / (A * pow(tempC, B))) / D;

  if (x <= 0.0f) return 0.0f;

  float windMPH = pow(x, C);
  return windMPH * 0.44704f; // mph -> m/s
}

// ================= JSON CALIBRATION =================
void fetchCalibration() {
  WiFiClientSecure https;
  https.setInsecure();
  HTTPClient http;

  http.begin(https, calibration_url);
  if (http.GET() == 200) {
    StaticJsonDocument<4096> doc;
    String payload = http.getString();

    if (!deserializeJson(doc, payload)) {
      if (doc["devices"].containsKey(deviceId)) {
        JsonObject dev = doc["devices"][deviceId];

        // New Rev P model only needs zeroVolts
        wind_zero = dev["wind"]["zeroVolts"] | 1.346f;

        noise_offset   = dev["noise"]["dbOffset"] | 0.0f;
        scd_temp_offset = dev["scd41"]["tempOffset"] | 0.0f;
        scd_hum_offset  = dev["scd41"]["humidityOffset"] | 0.0f;
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
  ads.setGain(GAIN_ONE);   // ADS1115: 0.125 mV/bit for single-ended conversion

  max31865.begin(MAX31865_4WIRE);

  Serial.println("System started");
  Serial.print("Device ID: ");
  Serial.println(deviceId);
  Serial.print("Wind zero voltage: ");
  Serial.println(wind_zero, 4);
}

// ================= LOOP =================
void loop() {
  if (!mqttClient.connected()) reconnectMQTT();
  mqttClient.loop();

  if (millis() - lastSensorRead >= 5000) {
    lastSensorRead = millis();

    char msgBuffer[20];

    // ================= SCD41 =================
    if (scd41.getDataReadyStatus() && scd41.readMeasurement()) {
      float temp = scd41.getTemperature() + scd_temp_offset;
      float hum  = scd41.getHumidity() + scd_hum_offset;
      uint16_t co2 = scd41.getCO2();

      dtostrf(temp, 5, 2, msgBuffer);
      mqttClient.publish((topic_base + "temp").c_str(), msgBuffer);

      dtostrf(hum, 5, 1, msgBuffer);
      mqttClient.publish((topic_base + "humidity").c_str(), msgBuffer);

      snprintf(msgBuffer, sizeof(msgBuffer), "%u", co2);
      mqttClient.publish((topic_base + "co2").c_str(), msgBuffer);
    }

    // ================= LIGHT =================
    OPT3001 result = opt3001.readResult();
    dtostrf(result.lux, 6, 0, msgBuffer);
    mqttClient.publish((topic_base + "lux").c_str(), msgBuffer);

    // ================= GLOBE =================
    float globeTemp = max31865.temperature(RNOMINAL, RREF);
    dtostrf(globeTemp, 5, 2, msgBuffer);
    mqttClient.publish((topic_base + "globe").c_str(), msgBuffer);

    // ================= WIND SENSOR (REV P NEW REGRESSION) =================
    // OUT = A0, TMP = A1
    int16_t adc0 = ads.readADC_SingleEnded(0);
    int16_t adc1 = ads.readADC_SingleEnded(1);

    float voltageOUT = adc0 * 0.000125f;   // GAIN_ONE => 125 uV/bit
    float voltageTMP = adc1 * 0.000125f;

    // TMP formula from sensor documentation
    float tempTMP = (voltageTMP - 0.400f) / 0.0195f;

    // Compute wind speed in m/s with Rev P regression
    float windSpeedMS = computeWindMS_RevP(voltageOUT, tempTMP, wind_zero);

    dtostrf(windSpeedMS, 5, 2, msgBuffer);
    mqttClient.publish((topic_base + "wind").c_str(), msgBuffer);

    dtostrf(tempTMP, 5, 2, msgBuffer);
    mqttClient.publish((topic_base + "wind_temp").c_str(), msgBuffer);

    // Optional raw debug channels
    dtostrf(voltageOUT, 5, 3, msgBuffer);
    mqttClient.publish((topic_base + "wind_out_volts").c_str(), msgBuffer);

    dtostrf(voltageTMP, 5, 3, msgBuffer);
    mqttClient.publish((topic_base + "wind_tmp_volts").c_str(), msgBuffer);

    // Serial debug
    Serial.print("OUT=");
    Serial.print(voltageOUT, 4);
    Serial.print(" V, TMP=");
    Serial.print(voltageTMP, 4);
    Serial.print(" V, Temp=");
    Serial.print(tempTMP, 2);
    Serial.print(" C, Wind=");
    Serial.print(windSpeedMS, 2);
    Serial.println(" m/s");

    if (voltageTMP < 0.30f || voltageTMP > 1.50f) {
      Serial.println("[WARN] Wind TMP voltage out of expected range");
    }
    if (voltageOUT < 0.20f || voltageOUT > 2.30f) {
      Serial.println("[WARN] Wind OUT voltage unusual for still/low wind");
    }

    // ================= NOISE =================
    int16_t adc2 = ads.readADC_SingleEnded(2);
    float voltage2 = adc2 * 0.000125f;
    float noiseDb = (voltage2 * 50.0f) + noise_offset;

    dtostrf(noiseDb, 5, 1, msgBuffer);
    mqttClient.publish((topic_base + "noise").c_str(), msgBuffer);
  }
}
