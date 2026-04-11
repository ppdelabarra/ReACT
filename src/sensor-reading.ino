#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <HTTPUpdate.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// -------- VERSION CONTROL --------
const char* currentVersion = "0.0";
const char* version_url = "https://raw.githubusercontent.com/ppdelabarra/ReACT/main/version.json";

// -------- MQTT --------
const char* mqtt_server = "7e43d9945d5748a782e8a2c3257f6743.s1.eu.hivemq.cloud";
const int   mqtt_port   = 8883;
const char* mqtt_user   = "ReACT";
const char* mqtt_pass   = "ThermalComfort2026";

const char* topic_wind = "sensor/wind_speed";

// -------- SENSOR --------
const int OutPin = 36;
const float ZERO_VOLTS = 1.075;
const float V_REF = 3.34;
const float ADC_RES = 4096.0;

// -------- CLIENTS --------
WiFiClientSecure espClient;
PubSubClient client(espClient);

// -------- OTA UPDATE FUNCTION --------
void updateFirmware(String url) {
    Serial.println("Starting OTA update...");
    Serial.println(url);

    WiFiClientSecure client;
    client.setInsecure();

    t_httpUpdate_return ret = httpUpdate.update(client, url);

    switch (ret) {
        case HTTP_UPDATE_FAILED:
            Serial.printf("Update failed (%d): %s\n",
                httpUpdate.getLastError(),
                httpUpdate.getLastErrorString().c_str());
            break;

        case HTTP_UPDATE_NO_UPDATES:
            Serial.println("No updates available.");
            break;

        case HTTP_UPDATE_OK:
            Serial.println("Update successful. Rebooting...");
            break;
    }
}

// -------- VERSION CHECK --------
void checkForUpdate() {
    Serial.println("Checking for updates...");

    HTTPClient http;
    http.begin(version_url);

    int httpCode = http.GET();

    if (httpCode == 200) {
        String payload = http.getString();

        StaticJsonDocument<512> doc;
        DeserializationError error = deserializeJson(doc, payload);

        if (!error) {
            String latestVersion = doc["version"];
            String binUrl = doc["bin"];

            Serial.print("Current version: ");
            Serial.println(currentVersion);

            Serial.print("Latest version: ");
            Serial.println(latestVersion);

            if (latestVersion != currentVersion) {
                Serial.println("New version found! Updating...");

                client.disconnect();
                delay(1000);

                updateFirmware(binUrl);
            } else {
                Serial.println("Already up to date.");
            }
        } else {
            Serial.println("JSON parsing failed");
        }
    } else {
        Serial.print("HTTP error: ");
        Serial.println(httpCode);
    }

    http.end();
}

// -------- MQTT RECONNECT --------
void reconnect() {
    while (!client.connected()) {
        Serial.print("Connecting to MQTT...");

        String clientId = "WindSensor-" + WiFi.macAddress();

        if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
            Serial.println("Connected!");
        } else {
            Serial.print("Failed, rc=");
            Serial.print(client.state());
            Serial.println(" retrying...");
            delay(5000);
        }
    }
}

// -------- SETUP --------
void setup() {
    Serial.begin(115200);

    WiFi.mode(WIFI_STA);
    delay(500);

    // WiFi Manager
    WiFiManager wm;
    if (!wm.autoConnect("ESP32_Wind_Setup")) {
        Serial.println("WiFi failed. Restarting...");
        ESP.restart();
    }

    Serial.println("WiFi Connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    Serial.print("MAC: ");
    Serial.println(WiFi.macAddress());

    // MQTT
    espClient.setInsecure();
    client.setServer(mqtt_server, mqtt_port);

    // OTA (local network)
    ArduinoOTA.setHostname("ESP32-Wind-Sensor");
    ArduinoOTA.begin();

    Serial.println("System Ready.");

    // Initial update check
    checkForUpdate();
}

// -------- LOOP --------
void loop() {
    ArduinoOTA.handle();

    if (!client.connected()) {
        reconnect();
    }
    client.loop();

    // -------- PERIODIC VERSION CHECK --------
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck > 60000) { // every 60 seconds
        lastCheck = millis();
        checkForUpdate();
    }

    // -------- SENSOR --------
    static unsigned long lastSensorRead = 0;

    if (millis() - lastSensorRead > 2000) {
        lastSensorRead = millis();

        float windVolts = (float)analogRead(OutPin) * (V_REF / ADC_RES);
        float windMS = 0.0;

        if (windVolts > ZERO_VOLTS) {
            float windMPH = pow(((windVolts - ZERO_VOLTS) / 0.2300), 2.7265);
            windMS = windMPH * 0.44704;
        }

        char msg[10];
        dtostrf(windMS, 4, 2, msg);
        client.publish(topic_wind, msg);

        Serial.print("Wind Speed: ");
        Serial.print(windMS);
        Serial.println(" m/s");
    }
}
