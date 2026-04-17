// ================= INCLUDES =================
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <Preferences.h>

#include <SparkFun_SCD4x_Arduino_Library.h>
#include <ClosedCube_OPT3001.h>
#include <Adafruit_ADS1X15.h>
#include <Adafruit_MAX31865.h>

// ================= GITHUB / OTA CONFIG =================
const char* version_url = "https://raw.githubusercontent.com/ppdelabarra/ReACT/main/version.json";
const char* calibration_url = "https://raw.githubusercontent.com/ppdelabarra/ReACT/main/config/calibration.json";

// ================= GLOBALS =================
Preferences preferences;

String currentVersion;
String deviceId;

unsigned long lastSensorRead = 0;
unsigned long lastOTAcheck = 0;

// ================= MQTT CONFIG =================
String mqttServer;
uint16_t mqttPort = 8883;
String mqttUser;
String mqttPass;

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
#define RREF     430.0
#define RNOMINAL 100.0

Adafruit_MAX31865 max31865(MAX_CS, MAX_MOSI, MAX_MISO, MAX_SCK);
SCD4x scd41;
Adafruit_ADS1115 ads;

// ================= CALIBRATION GLOBALS =================
float wind_zero = 1.346f;
float noise_offset = 0.0f;
float scd_temp_offset = 0.0f;
float scd_hum_offset = 0.0f;

// ================= MQTT STORAGE HELPERS =================
bool loadMqttConfig() {
  mqttServer = preferences.getString("mqtt_server", "");
  mqttPort   = preferences.getUInt("mqtt_port", 8883);
  mqttUser   = preferences.getString("mqtt_user", "");
  mqttPass   = preferences.getString("mqtt_pass", "");
  return mqttServer.length() > 0;
}

void saveMqttConfig(const String& server, uint16_t port, const String& user, const String& pass) {
  preferences.putString("mqtt_server", server);
  preferences.putUInt("mqtt_port", port);
  preferences.putString("mqtt_user", user);
  preferences.putString("mqtt_pass", pass);
}

// ================= PUBLISH HELPER =================
bool publishValue(const String& topic, const char* payload, bool retained = false) {
  bool ok = mqttClient.publish(topic.c_str(), payload, retained);
  if (!ok) {
    Serial.print("[WARN] Publish failed: ");
    Serial.println(topic);
  }
  return ok;
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

// ================= MQTT CALLBACK =================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  if (String(topic) == command_topic && msg == "calibrate_co2") {
    Serial.println("Received calibrate_co2 command");
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
    Serial.println("Attempting MQTT connection...");

    String clientId = "Station-" + deviceId;

    if (mqttClient.connect(clientId.c_str(), mqttUser.c_str(), mqttPass.c_str())) {
      Serial.println("MQTT connected");
      publishValue(topic_base + "status", "online", true);
      mqttClient.subscribe(command_topic.c_str());
    } else {
      Serial.print("MQTT failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" retrying in 3 seconds");
      delay(3000);
    }
  }
}

// ================= WIND REGRESSION =================
float computeWindMS_RevP(float outVolts, float tempC, float zeroWindVolts) {
  const float A = 3.038517f;
  const float B = 0.115157f;
  const float C = 3.009364f;
  const float D = 0.087288f;

  if (tempC < 0.1f) tempC = 0.1f;

  float x = ((outVolts - zeroWindVolts) / (A * pow(tempC, B))) / D;
  if (x <= 0.0f) return 0.0f;

  float windMPH = pow(x, C);
  return windMPH * 0.44704f;
}

// ================= FETCH CALIBRATION =================
void fetchCalibration() {
  WiFiClientSecure https;
  https.setInsecure();

  HTTPClient http;

  Serial.print("Fetching calibration from: ");
  Serial.println(calibration_url);

  http.begin(https, calibration_url);
  int code = http.GET();

  if (code == 200) {
    StaticJsonDocument<4096> doc;
    String payload = http.getString();
    DeserializationError err = deserializeJson(doc, payload);

    if (!err) {
      if (doc["devices"].containsKey(deviceId)) {
        JsonObject dev = doc["devices"][deviceId];

        wind_zero       = dev["wind"]["zeroVolts"] | 1.346f;
        noise_offset    = dev["noise"]["dbOffset"] | 0.0f;
        scd_temp_offset = dev["scd41"]["tempOffset"] | 0.0f;
        scd_hum_offset  = dev["scd41"]["humidityOffset"] | 0.0f;

        Serial.println("Calibration loaded for device.");
      } else {
        Serial.println("No device-specific calibration found; using defaults.");
      }
    } else {
      Serial.print("Calibration JSON parse failed: ");
      Serial.println(err.c_str());
    }
  } else {
    Serial.print("Calibration HTTP GET failed, code=");
    Serial.println(code);
  }

  http.end();
}

// ================= WIFI + MQTT SETUP PORTAL =================
void setupConnectivity() {
  WiFi.mode(WIFI_STA);

  WiFiManager wm;

  char mqttServerBuf[80] = "";
  char mqttPortBuf[8] = "8883";
  char mqttUserBuf[40] = "";
  char mqttPassBuf[80] = "";

  String savedServer = preferences.getString("mqtt_server", "");
  uint16_t savedPort = preferences.getUInt("mqtt_port", 8883);
  String savedUser   = preferences.getString("mqtt_user", "");
  String savedPass   = preferences.getString("mqtt_pass", "");

  savedServer.toCharArray(mqttServerBuf, sizeof(mqttServerBuf));
  String(savedPort).toCharArray(mqttPortBuf, sizeof(mqttPortBuf));
  savedUser.toCharArray(mqttUserBuf, sizeof(mqttUserBuf));
  savedPass.toCharArray(mqttPassBuf, sizeof(mqttPassBuf));

  WiFiManagerParameter custom_mqtt_server("server", "MQTT server", mqttServerBuf, sizeof(mqttServerBuf));
  WiFiManagerParameter custom_mqtt_port("port", "MQTT port", mqttPortBuf, sizeof(mqttPortBuf));
  WiFiManagerParameter custom_mqtt_user("user", "MQTT user", mqttUserBuf, sizeof(mqttUserBuf));
  WiFiManagerParameter custom_mqtt_pass("pass", "MQTT password", mqttPassBuf, sizeof(mqttPassBuf));

  wm.addParameter(&custom_mqtt_server);
  wm.addParameter(&custom_mqtt_port);
  wm.addParameter(&custom_mqtt_user);
  wm.addParameter(&custom_mqtt_pass);

  wm.setConfigPortalTimeout(180);

  if (!wm.autoConnect("ReACT_Station_Setup")) {
    Serial.println("WiFiManager failed or timed out, restarting...");
    delay(3000);
    ESP.restart();
  }

  mqttServer = String(custom_mqtt_server.getValue());
  mqttPort   = (uint16_t) String(custom_mqtt_port.getValue()).toInt();
  mqttUser   = String(custom_mqtt_user.getValue());
  mqttPass   = String(custom_mqtt_pass.getValue());

  if (mqttPort == 0) mqttPort = 8883;

  saveMqttConfig(mqttServer, mqttPort, mqttUser, mqttPass);

  Serial.println("WiFi connected");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(500);

  preferences.begin("react_system", false);

  loadMqttConfig();
  setupConnectivity();

  deviceId = WiFi.macAddress();
  topic_base = "sensor/" + deviceId + "/";
  command_topic = topic_base + "command";

  espClient.setInsecure();
  mqttClient.setServer(mqttServer.c_str(), mqttPort);
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

  Serial.println("System started");
  Serial.print("Device ID: ");
  Serial.println(deviceId);
  Serial.print("Topic base: ");
  Serial.println(topic_base);
  Serial.print("Wind zero voltage: ");
  Serial.println(wind_zero, 4);
}

// ================= LOOP =================
void loop() {
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }

  mqttClient.loop();

  if (millis() - lastSensorRead >= 5000) {
    lastSensorRead = millis();

    char msgBuffer[24];

    // ================= SCD41 =================
    if (scd41.getDataReadyStatus()) {
      if (scd41.readMeasurement()) {
        float temp = scd41.getTemperature() + scd_temp_offset;
        float hum  = scd41.getHumidity() + scd_hum_offset;
        uint16_t co2 = scd41.getCO2();

        dtostrf(temp, 5, 2, msgBuffer);
        publishValue(topic_base + "temp", msgBuffer);

        dtostrf(hum, 5, 1, msgBuffer);
        publishValue(topic_base + "humidity", msgBuffer);

        snprintf(msgBuffer, sizeof(msgBuffer), "%u", co2);
        publishValue(topic_base + "co2", msgBuffer);
      } else {
        Serial.println("[WARN] Failed reading SCD41");
      }
    }

    // ================= LIGHT =================
    OPT3001 result = opt3001.readResult();
    dtostrf(result.lux, 6, 0, msgBuffer);
    publishValue(topic_base + "lux", msgBuffer);

    // ================= GLOBE =================
    float globeTemp = max31865.temperature(RNOMINAL, RREF);
    dtostrf(globeTemp, 5, 2, msgBuffer);
    publishValue(topic_base + "globe", msgBuffer);

    // ================= WIND SENSOR =================
    int16_t adc0 = ads.readADC_SingleEnded(0);
    int16_t adc1 = ads.readADC_SingleEnded(1);

    float voltageOUT = adc0 * 0.000125f;
    float voltageTMP = adc1 * 0.000125f;

    float tempTMP = (voltageTMP - 0.400f) / 0.0195f;
    float windSpeedMS = computeWindMS_RevP(voltageOUT, tempTMP, wind_zero);

    dtostrf(windSpeedMS, 5, 2, msgBuffer);
    publishValue(topic_base + "wind", msgBuffer);

    dtostrf(tempTMP, 5, 2, msgBuffer);
    publishValue(topic_base + "wind_temp", msgBuffer);

    dtostrf(voltageOUT, 5, 3, msgBuffer);
    publishValue(topic_base + "wind_out_volts", msgBuffer);

    dtostrf(voltageTMP, 5, 3, msgBuffer);
    publishValue(topic_base + "wind_tmp_volts", msgBuffer);

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
    publishValue(topic_base + "noise", msgBuffer);
  }
}
