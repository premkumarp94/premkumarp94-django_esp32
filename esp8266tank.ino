#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

//==================================================
// WIFI
//==================================================

const char *ssid = "TIC_5G-PREM";
const char *password = "prem@123";

//==================================================
// DEVICE
//==================================================

const char *DEVICE_ID = "esp8266_device_01";

String pendingDeviceMsg = "esp8266 booted normally";

int iterationCount = 0;

//==================================================
// WATER PROBES & WIRE COLORS
//==================================================
// RED    = COM (Reference probe)
// BLACK  = D6  (100% Probe)
// GREEN  = D5  (75% Probe)
// YELLOW = D2  (50% Probe)
// BLUE   = D1  (25% Probe)

const int PROBE_25  = D1; // Blue
const int PROBE_50  = D2; // Yellow
const int PROBE_75  = D5; // Green
const int PROBE_100 = D6; // Black

// Using ESP8266 internal pull-up resistors.
// Probe in air   = HIGH
// Probe in water = LOW
const bool USE_INTERNAL_PULLUP = true;

//==================================================
// SERVERS
//==================================================

const char *serverList[] = {
    "https://premkumarp94.pythonanywhere.com/api/telemetry/"};

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
  Serial.print("Connecting WiFi");

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
// WATER LEVEL
//==================================================

// Individual probe detection states
bool probe25Detected = false;
bool probe50Detected = false;
bool probe75Detected = false;
bool probe100Detected = false;

int readWaterLevel() {
  // 1. Pre-charge probe pins to HIGH (3.3V) briefly
  pinMode(PROBE_25, OUTPUT);
  digitalWrite(PROBE_25, HIGH);
  pinMode(PROBE_50, OUTPUT);
  digitalWrite(PROBE_50, HIGH);
  pinMode(PROBE_75, OUTPUT);
  digitalWrite(PROBE_75, HIGH);
  pinMode(PROBE_100, OUTPUT);
  digitalWrite(PROBE_100, HIGH);
  delay(5);

  // 2. Switch pins to High-Impedance INPUT mode (no pull-up resistor loading)
  pinMode(PROBE_25, INPUT);
  pinMode(PROBE_50, INPUT);
  pinMode(PROBE_75, INPUT);
  pinMode(PROBE_100, INPUT);

  // Allow high-resistance water path to discharge charge to GND reference
  delay(30);

  // 3. Read probe states (In water: charge bleeds to GND -> pin reads LOW. In
  // air: stays HIGH)
  probe25Detected = (digitalRead(PROBE_25) == LOW);
  probe50Detected = (digitalRead(PROBE_50) == LOW);
  probe75Detected = (digitalRead(PROBE_75) == LOW);
  probe100Detected = (digitalRead(PROBE_100) == LOW);

  Serial.println();
  Serial.println("===== WATER PROBE TEST =====");

  Serial.print("25%  (D1 / BLUE)   = ");
  Serial.println(probe25Detected ? "DETECTED (WATER)" : "OPEN (AIR)");

  Serial.print("50%  (D2 / YELLOW) = ");
  Serial.println(probe50Detected ? "DETECTED (WATER)" : "OPEN (AIR)");

  Serial.print("75%  (D5 / GREEN)  = ");
  Serial.println(probe75Detected ? "DETECTED (WATER)" : "OPEN (AIR)");

  Serial.print("100% (D6 / BLACK)  = ");
  Serial.println(probe100Detected ? "DETECTED (WATER)" : "OPEN (AIR)");

  // Highest detected probe determines the level.
  if (probe100Detected) {
    return 100;
  }

  if (probe75Detected) {
    return 75;
  }

  if (probe50Detected) {
    return 50;
  }

  if (probe25Detected) {
    return 25;
  }

  return 0;
}

//==================================================
// HTTP POST JSON
//==================================================

bool postJson(const char *url, const String &body, String &responseOut) {
  responseOut = "";

  HTTPClient http;

  int httpCode = -1;

  Serial.print("POST: ");
  Serial.println(url);

  if (strncmp(url, "https://", 8) == 0) {
    WiFiClientSecure client;

    // Testing only.
    // This skips certificate verification.
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
  Serial.println("================================");
  Serial.println("Searching for telemetry server");
  Serial.println("================================");

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

      Serial.print("Discovery response: ");
      Serial.println(response);

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

String buildTelemetry(const String &ack, const String &message) {
  StaticJsonDocument<512> doc;

  doc["id"] = DEVICE_ID;

  JsonObject sensor = doc.createNestedObject("sensor values");

  int waterLevel = readWaterLevel();

  sensor["water_level"] = waterLevel;
  sensor["probe_25"] = probe25Detected;
  sensor["probe_50"] = probe50Detected;
  sensor["probe_75"] = probe75Detected;
  sensor["probe_100"] = probe100Detected;

  doc["ack"] = ack;

  doc["message"] = message;

  String body;

  serializeJson(doc, body);

  return body;
}

//==================================================
// SEND NORMAL TELEMETRY
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

  // Clear message only after successful transmission
  pendingDeviceMsg = "";

  Serial.println();
  Serial.println("Server response:");

  Serial.println(response);

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
  Serial.println("ESP8266 WATER LEVEL DEVICE STARTING");
  Serial.println("====================================");

  Serial.print("Device ID: ");
  Serial.println(DEVICE_ID);

  //================================================
  // WATER PROBES (Default: OFF / High-Impedance)
  //================================================

  pinMode(PROBE_25, INPUT);
  pinMode(PROBE_50, INPUT);
  pinMode(PROBE_75, INPUT);
  pinMode(PROBE_100, INPUT);

  //================================================
  // WIFI
  //================================================

  if (connectWiFi()) {
    discoverServer();
  }
}

//==================================================
// LOOP
//==================================================

void loop() {
  // Make sure WiFi is available
  if (!connectWiFi()) {
    delay(2000);

    return;
  }

  //================================================
  // NORMAL TELEMETRY
  //================================================

  sendTelemetry();

  //================================================
  // ITERATION MESSAGE
  //================================================

  if (iterationCount == 0) {
    pendingDeviceMsg = "came to first iteration";
  }

  iterationCount++;

  Serial.println();
  Serial.print("Iteration: ");
  Serial.println(iterationCount);

  Serial.println("------------------------------------");

  // Send every 5 seconds
  delay(5000);
}