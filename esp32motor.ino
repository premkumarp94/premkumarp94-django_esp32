#include <ESP32Servo.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

//==================================================
// HARDWARE & SERVO CONFIGURATION (ESP32)
// SG90 Servo Signal Line connected to Pin D13 (GPIO 13)
//==================================================
const int SERVO_PIN = 13;

// Angle definitions:
// Startup / Default: 90 degrees
// Motor ON: 45 degrees
// Motor OFF: 135 degrees
const int ANGLE_DEFAULT = 90;
const int ANGLE_ON      = 45;
const int ANGLE_OFF     = 135;

Servo motorServo;

// Motor State Tracking
bool motorRunning  = false;
int currentAngle   = ANGLE_DEFAULT;
String motorStatus = "OFF";

//==================================================
// WIFI CREDENTIALS & DEVICE IDENTIFIER
//==================================================
const char *ssid     = "TIC_5G-PREM";
const char *password = "prem@123";

const char *DEVICE_ID = "esp32motor";

// Candidate Server endpoints (Local PC LAN & Cloud PythonAnywhere fallback)
const char *serverList[] = {
    "http://192.168.1.7:8000/api/telemetry/",
    "http://192.168.1.2:8000/api/telemetry/",
    "http://192.168.1.3:8000/api/telemetry/",
    "http://192.168.1.4:8000/api/telemetry/",
    "http://192.168.1.5:8000/api/telemetry/",
    "https://premkumarp94.pythonanywhere.com/api/telemetry/"
};
const int SERVER_COUNT = sizeof(serverList) / sizeof(serverList[0]);
int activeServer = -1;

String pendingMsg = "esp32motor booted - default angle set to 90 deg";

// Forward declaration
void processCommand(const String &cmd);

//==================================================
// WIFI CONNECTION
//==================================================
bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  Serial.println();
  Serial.print("[esp32motor] Connecting to WiFi (SSID: ");
  Serial.print(ssid);
  Serial.println(")...");

  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[esp32motor] WiFi Connected! IP Address: ");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.println("[esp32motor] WiFi Connection Failed.");
  return false;
}

//==================================================
// HTTP POST JSON HELPER
//==================================================
bool postJson(const char *url, const String &body, String &responseOut) {
  responseOut = "";
  HTTPClient http;
  int httpCode = -1;

  Serial.print("[HTTP POST] Connecting to: ");
  Serial.println(url);

  if (strncmp(url, "https://", 8) == 0) {
    WiFiClientSecure client;
    client.setInsecure();
    if (http.begin(client, url)) {
      http.addHeader("Content-Type", "application/json");
      http.setTimeout(5000);
      httpCode = http.POST(body);
      if (httpCode > 0) responseOut = http.getString();
      http.end();
    }
  } else {
    WiFiClient client;
    if (http.begin(client, url)) {
      http.addHeader("Content-Type", "application/json");
      http.setTimeout(5000);
      httpCode = http.POST(body);
      if (httpCode > 0) responseOut = http.getString();
      http.end();
    }
  }

  Serial.printf("[HTTP Code] Status: %d for URL: %s\n", httpCode, url);
  return (httpCode >= 200 && httpCode < 300);
}

//==================================================
// SERVER DISCOVERY
//==================================================
bool discoverServer() {
  Serial.println("\n[esp32motor] Searching candidate servers for active Django API...");

  for (int i = 0; i < SERVER_COUNT; i++) {
    StaticJsonDocument<256> doc;
    doc["id"] = DEVICE_ID;
    doc["motor_status"] = motorStatus;
    doc["motor_running"] = motorRunning;
    doc["servo_angle"] = currentAngle;
    doc["message"] = "ping";

    String body, response;
    serializeJson(doc, body);

    if (postJson(serverList[i], body, response)) {
      activeServer = i;
      Serial.print("[esp32motor] Connected & locked active server: ");
      Serial.println(serverList[i]);
      return true;
    }
  }

  activeServer = -1;
  Serial.println("[esp32motor] No candidate server responded.");
  return false;
}

//==================================================
// SEND MOTOR TELEMETRY / STATUS TO DJANGO SERVER
//==================================================
bool sendMotorStatusToServer(const String &extraMessage = "") {
  if (activeServer == -1) {
    if (!discoverServer()) return false;
  }

  StaticJsonDocument<512> doc;
  doc["id"] = DEVICE_ID;
  doc["motor_status"] = motorStatus;
  doc["motor_running"] = motorRunning;
  doc["servo_angle"] = currentAngle;
  doc["ack"] = "esp32motor_ok";

  if (extraMessage.length() > 0) {
    doc["message"] = extraMessage;
  } else if (pendingMsg.length() > 0) {
    doc["message"] = pendingMsg;
    pendingMsg = "";
  } else {
    doc["message"] = "esp32motor telemetry sync";
  }

  String body, response;
  serializeJson(doc, body);

  Serial.println("\n--- [esp32motor] Transmitting Motor Status to Django Server ---");
  Serial.println(body);

  bool success = postJson(serverList[activeServer], body, response);

  if (!success) {
    Serial.println("[esp32motor] Active server failed. Re-initiating discovery...");
    activeServer = -1;
    return false;
  }

  Serial.println("Server Response:");
  Serial.println(response);

  // Parse server response to process queued motor commands
  StaticJsonDocument<512> respDoc;
  DeserializationError err = deserializeJson(respDoc, response);
  if (!err && respDoc.containsKey("command")) {
    String cmd = respDoc["command"].as<String>();
    cmd.trim();
    cmd.toUpperCase();
    if (cmd.length() > 0) {
      Serial.print("[COMMAND DELIVERED] Executing command: ");
      Serial.println(cmd);
      processCommand(cmd);
    }
  }

  return true;
}

//==================================================
// MOTOR CONTROL ACTIONS (SG90 Servo on Pin D13)
//==================================================

void turnMotorON() {
  Serial.println("\n>>> TURNING MOTOR ON >>> Rotating SG90 Servo to 45 degrees");
  currentAngle = ANGLE_ON;   // 45 degrees
  motorRunning = true;
  motorStatus  = "ON";

  motorServo.write(ANGLE_ON);
  delay(350); // Allow physical servo arm to rotate to position

  // Immediately notify server after turning ON!
  sendMotorStatusToServer("motor turned ON (servo rotated to 45 deg)");
}

void turnMotorOFF() {
  Serial.println("\n>>> TURNING MOTOR OFF >>> Rotating SG90 Servo to 135 degrees");
  currentAngle = ANGLE_OFF;  // 135 degrees
  motorRunning = false;
  motorStatus  = "OFF";

  motorServo.write(ANGLE_OFF);
  delay(350); // Allow physical servo arm to rotate to position

  // Immediately notify server after turning OFF!
  sendMotorStatusToServer("motor turned OFF (servo rotated to 135 deg)");
}

void processCommand(const String &cmd) {
  if (cmd == "MOTOR_ON" || cmd == "ON" || cmd == "TURN_ON" || cmd == "START" || cmd == "45") {
    if (!motorRunning || currentAngle != ANGLE_ON) {
      turnMotorON();
    } else {
      Serial.println("[esp32motor] Motor is ALREADY ON (45 deg).");
    }
  } else if (cmd == "MOTOR_OFF" || cmd == "OFF" || cmd == "TURN_OFF" || cmd == "STOP" || cmd == "135") {
    if (motorRunning || currentAngle != ANGLE_OFF) {
      turnMotorOFF();
    } else {
      Serial.println("[esp32motor] Motor is ALREADY OFF (135 deg).");
    }
  }
}

//==================================================
// SETUP
//==================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("==================================================");
  Serial.println("ESP32 MOTOR PROGRAM (SG90 Servo @ Pin D13)");
  Serial.println("==================================================");

  // CAREFULLY SET DEFAULT ANGLE EVEN AT STARTUP TO 90 DEGREES
  motorServo.setPeriodHertz(50);             // Standard 50Hz servo PWM frequency
  motorServo.write(ANGLE_DEFAULT);           // Pre-set 90 deg position before attach
  motorServo.attach(SERVO_PIN, 500, 2400);   // Attach SG90 Servo to GPIO 13 (D13)
  motorServo.write(ANGLE_DEFAULT);           // Ensure angle is firmly 90 degrees at startup

  currentAngle = ANGLE_DEFAULT; // 90 degrees
  motorRunning = false;
  motorStatus  = "OFF";

  Serial.printf("Servo attached to GPIO 13 (D13). Startup default angle: %d deg\n", currentAngle);

  // Connect to WiFi network
  if (connectWiFi()) {
    discoverServer();
    // Notify server of startup status & default 90 deg angle
    sendMotorStatusToServer("esp32motor booted - default startup angle set to 90 deg");
  }
}

//==================================================
// MAIN LOOP
//==================================================
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  // Continuously sync telemetry and poll queued commands from server
  sendMotorStatusToServer();

  delay(5000); // Poll server every 5 seconds
}
