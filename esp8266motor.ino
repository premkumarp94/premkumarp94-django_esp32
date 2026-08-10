#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

//==================================================
// WIFI CONFIGURATION
//==================================================
const char* ssid = "TIC_5G-PREM";
const char* password = "prem@123";

//==================================================
// DEVICE CONFIGURATION
//==================================================
const char* DEVICE_ID = "esp8266_device_02";
String motorStatus = "stopped";
String pendingDeviceMsg = "esp8266_device_02 booted normally";
int iterationCount = 0;

//==================================================
// RELAY PIN DEFINITION
//==================================================
// D1 (GPIO5) controls the Relay for Water Pump Motor
const int RELAY_PIN = D1;

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
    client.setInsecure(); // Skip certificate verification for testing

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
// BUILD TELEMETRY JSON
//==================================================
String buildTelemetry(const String& ack, const String& message) {
  StaticJsonDocument<512> doc;

  doc["id"] = DEVICE_ID;

  JsonObject sensor = doc.createNestedObject("sensor values");
  sensor["motor_status"] = motorStatus;

  doc["ack"] = ack;
  doc["message"] = message;

  String body;
  serializeJson(doc, body);
  return body;
}

//==================================================
// APPLY MOTOR STATE TO RELAY
//==================================================
void setMotorState(bool start) {
  if (start) {
    digitalWrite(RELAY_PIN, HIGH);
    motorStatus = "started";
    Serial.println(">>> RELAY ON (MOTOR RUNNING) <<<");
  } else {
    digitalWrite(RELAY_PIN, LOW);
    motorStatus = "stopped";
    Serial.println(">>> RELAY OFF (MOTOR STOPPED) <<<");
  }
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

  String ack = "dummy_ack";
  String message = pendingDeviceMsg;

  String body = buildTelemetry(ack, message);

  Serial.println();
  Serial.println("Sending telemetry:");
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

  // Parse server command
  StaticJsonDocument<512> responseDoc;
  DeserializationError error = deserializeJson(responseDoc, response);

  if (error) {
    Serial.print("JSON parse error: ");
    Serial.println(error.c_str());
    return true;
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
  }

  if (commandExecuted) {
    Serial.println();
    Serial.println("Sending immediate command confirmation...");

    String confirmBody = buildTelemetry("cmd_executed_ack", pendingDeviceMsg);
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
  Serial.println("====================================");
  Serial.println("ESP8266 WATER MOTOR CONTROLLER STARTING");
  Serial.println("====================================");
  Serial.print("Device ID: ");
  Serial.println(DEVICE_ID);

  pinMode(RELAY_PIN, OUTPUT);
  setMotorState(false); // Default motor state: STOPPED

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
    pendingDeviceMsg = "came to first iteration";
  }

  iterationCount++;

  Serial.println();
  Serial.print("Iteration: ");
  Serial.println(iterationCount);
  Serial.println("------------------------------------");

  delay(5000);
}
