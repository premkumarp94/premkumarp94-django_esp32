#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>

//==================================================
// WIFI CONFIGURATION
//==================================================
const char* ssid = "TIC_5G-PREM";
const char* password = "prem@123";

//==================================================
// DEVICE CONFIGURATION & STATE
//==================================================
const char* DEVICE_ID = "esp8266_device_02";
String motorStatus = "stopped";
String pendingDeviceMsg = "esp8266motor booted normally";
int iterationCount = 0;

// Water Level & Settings Configuration
int minPercentage = 33;       // Minimum threshold to turn ON motor
int maxPercentage = 100;      // Maximum threshold to turn OFF motor
bool waterLevelMode = true;   // Water Level Mode (true = Auto mode ENABLED, false = MANUAL mode)
int currentWaterLevel = 0;    // Current measured water level (synced from Tank Unit esp8266_device_01 or local probes)

// IST Timestamp in YYYYMMDD:HH:MM:SS format
String currentIstTime = "20260811:00:00:00";

// Set to true ONLY if physical probe sensors are directly wired to THIS motor ESP8266 board.
// Set to false (default) if water probe sensors are on esp8266_device_01 (Tank Unit).
const bool ENABLE_LOCAL_PROBES = false;

// Track Arduino's Last Understood State (for Synchronization)
int lastArduinoMin = -1;
int lastArduinoMax = -1;
bool lastArduinoAuto = false;
int lastArduinoLevel = -1;
String lastArduinoMotor = "unknown";

//==================================================
// WATER PROBE PINS (ESP8266)
//==================================================
const int COMMON_PIN = D1; // Reference probe pin (GND output)
const int LOW_PIN    = D2; // Low level probe (33%)
const int MID_PIN    = D5; // Mid level probe (66%)
const int FULL_PIN   = D6; // Full level probe (100%)

const bool USE_INTERNAL_PULLUP = true;

//==================================================
// SERVERS LIST
//==================================================
const char* serverList[] = {
  "http://192.168.1.1:8000/api/telemetry/",
  "http://192.168.1.2:8000/api/telemetry/",
  "http://192.168.1.3:8000/api/telemetry/",
  "http://192.168.1.4:8000/api/telemetry/",
  "http://192.168.1.5:8000/api/telemetry/",
  "https://premkumarp94.pythonanywhere.com/api/telemetry/"
};

const int SERVER_COUNT = sizeof(serverList) / sizeof(serverList[0]);
int activeServer = -1;

//==================================================
// IST TIMESTAMP GENERATOR (YYYYMMDD:HH:MM:SS)
//==================================================
String getIstTimestamp() {
  time_t now = time(nullptr);
  if (now > 100000) {
    struct tm* timeinfo = localtime(&now);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d%02d%02d:%02d:%02d:%02d",
             timeinfo->tm_year + 1900,
             timeinfo->tm_mon + 1,
             timeinfo->tm_mday,
             timeinfo->tm_hour,
             timeinfo->tm_min,
             timeinfo->tm_sec);
    return String(buf);
  }
  return currentIstTime;
}

//==================================================
// WATER LEVEL PROBE READER
//==================================================
int readWaterLevel() {
  if (!ENABLE_LOCAL_PROBES) {
    // Probes are on esp8266_device_01 (Tank Unit). Use water level synced from Django telemetry.
    return currentWaterLevel;
  }

  if (USE_INTERNAL_PULLUP) {
    digitalWrite(COMMON_PIN, LOW);
    delay(30);

    bool low  = (digitalRead(LOW_PIN) == LOW);
    bool mid  = (digitalRead(MID_PIN) == LOW);
    bool full = (digitalRead(FULL_PIN) == LOW);

    Serial.println();
    Serial.println("===== LOCAL WATER PROBE READINGS =====");
    Serial.print("LOW  (D2)  = "); Serial.println(low ? "WATER DETECTED" : "OPEN AIR");
    Serial.print("MID  (D5)  = "); Serial.println(mid ? "WATER DETECTED" : "OPEN AIR");
    Serial.print("FULL (D6)  = "); Serial.println(full ? "WATER DETECTED" : "OPEN AIR");

    if (full) return 100;
    if (mid)  return 66;
    if (low)  return 33;
    return 0;
  } else {
    digitalWrite(COMMON_PIN, HIGH);
    delay(30);

    bool low  = (digitalRead(LOW_PIN) == HIGH);
    bool mid  = (digitalRead(MID_PIN) == HIGH);
    bool full = (digitalRead(FULL_PIN) == HIGH);

    digitalWrite(COMMON_PIN, LOW);

    if (full) return 100;
    if (mid)  return 66;
    if (low)  return 33;
    return 0;
  }
}

//==================================================
// DUAL JSON SERIAL TRANSMISSIONS TO ARDUINO WITH IST DATETIME
//==================================================

// 1. Send Settings JSON to Arduino
void sendSettingsJsonToArduino() {
  StaticJsonDocument<256> doc;
  doc["type"] = "settings";
  doc["min"] = minPercentage;
  doc["max"] = maxPercentage;
  doc["auto"] = waterLevelMode;
  doc["time"] = getIstTimestamp();

  String output;
  serializeJson(doc, output);
  Serial.println(output); // Sent to Arduino via Serial
}

// 2. Send Status/Percentage JSON to Arduino
void sendStatusJsonToArduino() {
  StaticJsonDocument<256> doc;
  doc["type"] = "status";
  doc["level"] = currentWaterLevel;
  doc["motor"] = motorStatus;
  doc["time"] = getIstTimestamp();

  String output;
  serializeJson(doc, output);
  Serial.println(output); // Sent to Arduino via Serial
}

// Set Motor State & Send Status JSON to Arduino
void setMotorState(bool start) {
  if (start) {
    motorStatus = "started";
  } else {
    motorStatus = "stopped";
  }
  sendStatusJsonToArduino();
}

// Check Arduino state JSON response & verify synchronization
void processArduinoSerialResponse() {
  while (Serial.available() > 0) {
    String line = Serial.readStringUntil('\n');
    line.trim();

    if (line.length() == 0) continue;

    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, line);

    if (!error) {
      String msgType = doc["type"] | "";
      if (msgType == "state") {
        lastArduinoMin = doc["min"] | -1;
        lastArduinoMax = doc["max"] | -1;
        lastArduinoAuto = doc["auto"] | false;
        lastArduinoLevel = doc["level"] | -1;
        lastArduinoMotor = doc["motor"] | "unknown";

        Serial.println("\n[ARDUINO STATE RECEIVED]");
        Serial.print("  Min: "); Serial.print(lastArduinoMin);
        Serial.print("% | Max: "); Serial.print(lastArduinoMax);
        Serial.print("% | Auto: "); Serial.print(lastArduinoAuto ? "ON" : "OFF");
        Serial.print(" | Level: "); Serial.print(lastArduinoLevel);
        Serial.print("% | Motor: "); Serial.println(lastArduinoMotor);

        // Verify if Arduino state matches ESP8266 knowledge
        bool inSync = (lastArduinoMin == minPercentage &&
                       lastArduinoMax == maxPercentage &&
                       lastArduinoAuto == waterLevelMode &&
                       lastArduinoLevel == currentWaterLevel &&
                       lastArduinoMotor == motorStatus);

        if (!inSync) {
          Serial.println("\n[SYNC MISMATCH DETECTED] Arduino state is out of sync with ESP8266 knowledge!");
          Serial.println(">>> INITIATING ESP8266 -> ARDUINO STATE SYNC <<<");
          sendSettingsJsonToArduino();
          delay(50);
          sendStatusJsonToArduino();
        } else {
          Serial.println("[SYNC STATUS] Arduino is fully in sync with ESP8266.");
        }
      }
    }
  }
}

//==================================================
// AUTO WATER LEVEL MODE EVALUATION
//==================================================
void evaluateAutoWaterLevelMode(int waterLevel) {
  if (!waterLevelMode) {
    Serial.println("Auto Water Level Mode is DISABLED (Manual Mode Active).");
    return;
  }

  Serial.print("Auto Water Level Mode ACTIVE. Tank Level: ");
  Serial.print(waterLevel);
  Serial.print("% | Target Range: [Min: ");
  Serial.print(minPercentage);
  Serial.print("%, Max: ");
  Serial.print(maxPercentage);
  Serial.println("%]");

  if (waterLevel <= minPercentage && motorStatus != "started") {
    Serial.println(">>> AUTO TRIGGER: Water Level <= Min Threshold! Starting Motor... <<<");
    setMotorState(true);
    pendingDeviceMsg = "Auto Trigger: Tank level (" + String(waterLevel) + "%) <= Min (" + String(minPercentage) + "%). Motor STARTED.";
  } else if (waterLevel >= maxPercentage && motorStatus != "stopped") {
    Serial.println(">>> AUTO TRIGGER: Water Level >= Max Threshold! Stopping Motor... <<<");
    setMotorState(false);
    pendingDeviceMsg = "Auto Trigger: Tank level (" + String(waterLevel) + "%) >= Max (" + String(maxPercentage) + "%). Motor STOPPED.";
  }
}

//==================================================
// WIFI CONNECTION
//==================================================
bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  Serial.println();
  Serial.print("Connecting WiFi to ");
  Serial.print(ssid);

  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected. IP: ");
    Serial.println(WiFi.localIP());
    // Configure NTP time for IST (UTC+5:30)
    configTime(5.5 * 3600, 0, "pool.ntp.org", "time.nist.gov");
    return true;
  }

  Serial.println("WiFi connection failed.");
  return false;
}

//==================================================
// HTTP POST JSON
//==================================================
bool postJson(const char* url, const String& body, String& responseOut) {
  responseOut = "";
  HTTPClient http;
  int httpCode = -1;

  Serial.print("POST: ");
  Serial.println(url);

  if (strncmp(url, "https://", 8) == 0) {
    WiFiClientSecure client;
    client.setInsecure();

    if (!http.begin(client, url)) {
      Serial.println("HTTPS begin failed.");
      return false;
    }

    http.addHeader("Content-Type", "application/json");
    http.setTimeout(5000);
    httpCode = http.POST(body);
    if (httpCode > 0) {
      responseOut = http.getString();
    }
    http.end();
  } else {
    WiFiClient client;
    if (!http.begin(client, url)) {
      Serial.println("HTTP begin failed.");
      return false;
    }

    http.addHeader("Content-Type", "application/json");
    http.setTimeout(5000);
    httpCode = http.POST(body);
    if (httpCode > 0) {
      responseOut = http.getString();
    }
    http.end();
  }

  Serial.print("HTTP code: ");
  Serial.println(httpCode);

  if (httpCode >= 200 && httpCode < 300) {
    return true;
  }

  if (httpCode > 0) {
    Serial.println("Server response:");
    Serial.println(responseOut);
  }

  return false;
}

//==================================================
// SERVER DISCOVERY
//==================================================
bool discoverServer() {
  Serial.println();
  Serial.println("====================================");
  Serial.println("Searching for telemetry server");
  Serial.println("====================================");

  for (int i = 0; i < SERVER_COUNT; i++) {
    StaticJsonDocument<256> doc;
    doc["id"] = DEVICE_ID;
    doc["message"] = "ping";

    String body;
    serializeJson(doc, body);

    String response;
    Serial.print("Trying ");
    Serial.println(serverList[i]);

    if (postJson(serverList[i], body, response)) {
      activeServer = i;
      Serial.println("SERVER CONNECTED!");
      Serial.print("Active server: ");
      Serial.println(serverList[i]);
      return true;
    }

    Serial.println("No response.");
  }

  activeServer = -1;
  Serial.println("No server available.");
  return false;
}

//==================================================
// BUILD TELEMETRY JSON WITH WATER LEVEL & SETTINGS
//==================================================
String buildTelemetry(const String& ack, const String& message, int waterLevel) {
  StaticJsonDocument<512> doc;

  doc["id"] = DEVICE_ID;

  JsonObject sensor = doc.createNestedObject("sensor values");
  sensor["water_level"] = waterLevel;
  sensor["motor_status"] = motorStatus;
  sensor["min_percentage"] = minPercentage;
  sensor["max_percentage"] = maxPercentage;
  sensor["water_level_mode"] = waterLevelMode;

  doc["ack"] = ack;
  doc["message"] = message;

  String body;
  serializeJson(doc, body);
  return body;
}

//==================================================
// SEND TELEMETRY & PROCESS COMMANDS
//==================================================
bool sendTelemetry() {
  if (activeServer == -1) {
    if (!discoverServer()) {
      return false;
    }
  }

  currentWaterLevel = readWaterLevel();

  // Evaluate Auto Water Level Mode rules before sending telemetry
  evaluateAutoWaterLevelMode(currentWaterLevel);

  // Send JSON messages over Serial to Arduino
  sendSettingsJsonToArduino();
  delay(50);
  sendStatusJsonToArduino();
  delay(50);
  processArduinoSerialResponse();

  String ack = "dummy_ack";
  String message = pendingDeviceMsg;

  String body = buildTelemetry(ack, message, currentWaterLevel);

  Serial.println();
  Serial.println("Sending telemetry payload to Django:");
  Serial.println(body);

  String response;
  bool success = postJson(serverList[activeServer], body, response);

  if (!success) {
    Serial.println("Active server failed.");
    activeServer = -1;
    return false;
  }

  pendingDeviceMsg = "";

  Serial.println();
  Serial.println("Server response:");
  Serial.println(response);

  // Parse server response command & settings
  StaticJsonDocument<512> responseDoc;
  DeserializationError error = deserializeJson(responseDoc, response);

  if (error) {
    Serial.print("JSON parse error: ");
    Serial.println(error.c_str());
    return true;
  }

  // Update IST Datetime, Tank Water Level & Thresholds from Django Server Response
  if (!responseDoc["ist_time"].isNull()) {
    currentIstTime = responseDoc["ist_time"].as<String>();
  }

  if (!responseDoc["water_level"].isNull() && !ENABLE_LOCAL_PROBES) {
    int serverLvl = responseDoc["water_level"].as<int>();
    if (serverLvl != currentWaterLevel) {
      currentWaterLevel = serverLvl;
      Serial.print("Synced Water Level from Tank Device esp8266_device_01: ");
      Serial.print(currentWaterLevel);
      Serial.println("%");
    }
  }

  if (!responseDoc["start_level"].isNull()) {
    minPercentage = responseDoc["start_level"].as<int>();
  }
  if (!responseDoc["stop_level"].isNull()) {
    maxPercentage = responseDoc["stop_level"].as<int>();
  }
  if (!responseDoc["auto_mode"].isNull()) {
    waterLevelMode = responseDoc["auto_mode"].as<bool>();
  }

  String command = responseDoc["command"] | "";
  Serial.print("Command received: ");
  Serial.println(command);

  bool commandExecuted = false;

  if (command == "start_motor" || command == "front") {
    setMotorState(true);
    pendingDeviceMsg = "Motor started successfully.";
    commandExecuted = true;
  } else if (command == "stop_motor" || command == "stop") {
    setMotorState(false);
    pendingDeviceMsg = "Motor stopped successfully.";
    commandExecuted = true;
  } else if (command.startsWith("set_threshold:")) {
    int firstColon = command.indexOf(':');
    int secondColon = command.indexOf(':', firstColon + 1);
    int thirdColon = command.indexOf(':', secondColon + 1);

    if (firstColon != -1 && secondColon != -1) {
      String startStr = command.substring(firstColon + 1, secondColon);
      String stopStr = (thirdColon != -1) ? command.substring(secondColon + 1, thirdColon) : command.substring(secondColon + 1);
      String autoStr = (thirdColon != -1) ? command.substring(thirdColon + 1) : "1";

      minPercentage = startStr.toInt();
      maxPercentage = stopStr.toInt();
      waterLevelMode = (autoStr.toInt() == 1 || autoStr.equalsIgnoreCase("true"));

      // Transmit updated settings JSON to Arduino immediately
      sendSettingsJsonToArduino();
      delay(50);
      processArduinoSerialResponse();

      pendingDeviceMsg = "Settings updated: Min=" + String(minPercentage) + "%, Max=" + String(maxPercentage) + "%, AutoMode=" + String(waterLevelMode ? "ON" : "OFF");
      Serial.println(pendingDeviceMsg);
      commandExecuted = true;
    }
  } else if (command == "auto_on") {
    waterLevelMode = true;
    sendSettingsJsonToArduino();
    pendingDeviceMsg = "Water Level Mode enabled (Auto mode ON).";
    commandExecuted = true;
  } else if (command == "auto_off") {
    waterLevelMode = false;
    sendSettingsJsonToArduino();
    pendingDeviceMsg = "Water Level Mode disabled (Manual mode ON).";
    commandExecuted = true;
  }

  // Send latest synced status & settings to Arduino
  sendSettingsJsonToArduino();
  delay(50);
  sendStatusJsonToArduino();

  if (commandExecuted) {
    Serial.println();
    Serial.println("Sending immediate command confirmation...");

    String confirmBody = buildTelemetry("cmd_executed_ack", pendingDeviceMsg, currentWaterLevel);
    Serial.println("Confirmation JSON:");
    Serial.println(confirmBody);

    String confirmResponse;
    bool confirmSuccess = postJson(serverList[activeServer], confirmBody, confirmResponse);

    if (confirmSuccess) {
      Serial.println("Server state updated immediately!");
      pendingDeviceMsg = "";
    } else {
      Serial.println("Immediate confirmation failed. Will retry next loop.");
    }
  }

  return true;
}

//==================================================
// SETUP
//==================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("==============================================");
  Serial.println("ESP8266 WATER MOTOR CONTROLLER STARTING");
  Serial.println("Sending Water Level & Settings to Server");
  Serial.println("Sending Dual JSON Serial Packets with IST Datetime to Arduino");
  Serial.println("==============================================");
  Serial.print("Device ID: ");
  Serial.println(DEVICE_ID);

  if (ENABLE_LOCAL_PROBES) {
    pinMode(COMMON_PIN, OUTPUT);
    if (USE_INTERNAL_PULLUP) {
      digitalWrite(COMMON_PIN, LOW);
      pinMode(LOW_PIN, INPUT_PULLUP);
      pinMode(MID_PIN, INPUT_PULLUP);
      pinMode(FULL_PIN, INPUT_PULLUP);
    } else {
      digitalWrite(COMMON_PIN, LOW);
      pinMode(LOW_PIN, INPUT);
      pinMode(MID_PIN, INPUT);
      pinMode(FULL_PIN, INPUT);
    }
  }

  // Default motor state: STOPPED
  setMotorState(false);

  if (connectWiFi()) {
    discoverServer();
  }
}

//==================================================
// LOOP
//==================================================
void loop() {
  if (!connectWiFi()) {
    delay(2000);
    return;
  }

  sendTelemetry();

  if (iterationCount == 0) {
    pendingDeviceMsg = "esp8266motor first iteration complete";
  }

  iterationCount++;

  Serial.println();
  Serial.print("Iteration: ");
  Serial.println(iterationCount);
  Serial.println("------------------------------------");

  delay(5000);
}
