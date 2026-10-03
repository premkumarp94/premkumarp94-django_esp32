#include <ESP32Servo.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

//==================================================
// HARDWARE & SERVO CONFIGURATION (ESP32)
// SG90 Servo Signal Line connected to Pin D13 (GPIO 13)
// 4 Water Level Indicator LEDs:
// - 25% Level LED:  GPIO 25 (Pin D25) [Blinks 0.5s ON / 0.5s OFF when empty]
// - 50% Level LED:  GPIO 26 (Pin D26)
// - 75% Level LED:  GPIO 27 (Pin D27)
// - 100% Level LED: GPIO 14 (Pin D14)
//==================================================
const int SERVO_PIN   = 13;
const int LED_25_PIN  = 25;
const int LED_50_PIN  = 26;
const int LED_75_PIN  = 27;
const int LED_100_PIN = 14;

// Angle definitions:
// Startup / Resting Default: 90 degrees
// Configurable ON Angle (default 45), OFF Angle (default 135), and Hold Duration (default 1000ms = 1s)
const int ANGLE_DEFAULT = 90;
int angleON             = 45;
int angleOFF            = 135;
int holdDurationMs      = 1000; // Configurable hold time in ms before returning to 90 deg (default 1000ms)

Servo motorServo;
Preferences preferences;

// Motor & Telemetry State Tracking
bool motorRunning     = false;
int currentAngle      = ANGLE_DEFAULT;
String motorStatus    = "OFF";
int currentWaterLevel = 0; // Synced water level from Django server

// Loop timer tracking
unsigned long lastSyncTime = 0;

//==================================================
// WIFI CREDENTIALS & DEVICE IDENTIFIER
//==================================================
const char *ssid     = "TIC_5G-PREM";
const char *password = "prem@123";

const char *DEVICE_ID = "esp32motor";

// Candidate Server endpoints (Local PC LAN & Cloud PythonAnywhere fallback)
const char *serverList[] = {
    "https://premkumarp94.pythonanywhere.com/api/telemetry/"
};
const int SERVER_COUNT = sizeof(serverList) / sizeof(serverList[0]);
int activeServer = -1;

String pendingMsg = "esp32motor booted - default angle set to 90 deg";

// Forward declarations
void processCommand(const String &cmd);
void updateWaterLevelLEDs();

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
    doc["angle_on"] = angleON;
    doc["angle_off"] = angleOFF;
    doc["hold_ms"] = holdDurationMs;
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
  doc["angle_on"] = angleON;
  doc["angle_off"] = angleOFF;
  doc["hold_ms"] = holdDurationMs;
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

  // Parse server response for water level & queued motor commands
  StaticJsonDocument<512> respDoc;
  DeserializationError err = deserializeJson(respDoc, response);
  if (!err) {
    if (respDoc.containsKey("water_level") && !respDoc["water_level"].isNull()) {
      currentWaterLevel = respDoc["water_level"].as<int>();
      Serial.printf("[esp32motor] Synced live water level: %d%%\n", currentWaterLevel);
    }
    if (respDoc.containsKey("command")) {
      String cmd = respDoc["command"].as<String>();
      cmd.trim();
      cmd.toUpperCase();
      if (cmd.length() > 0) {
        Serial.print("[COMMAND DELIVERED] Executing command: ");
        Serial.println(cmd);
        processCommand(cmd);
      }
    }
  }

  return true;
}

//==================================================
// 4-LED WATER LEVEL INDICATOR CONTROL
//==================================================
void updateWaterLevelLEDs() {
  static unsigned long lastBlinkTime = 0;
  static bool blinkState = false;
  unsigned long now = millis();

  // Non-blocking 0.5-second blink timer (500 ms ON / 500 ms OFF)
  if (now - lastBlinkTime >= 500) {
    lastBlinkTime = now;
    blinkState = !blinkState;
  }

  if (currentWaterLevel <= 0) {
    // TANK EMPTY (0%): Blink 25% LED (0.5s ON / 0.5s OFF), other LEDs OFF
    digitalWrite(LED_25_PIN, blinkState ? HIGH : LOW);
    digitalWrite(LED_50_PIN, LOW);
    digitalWrite(LED_75_PIN, LOW);
    digitalWrite(LED_100_PIN, LOW);
  } else {
    // Tank has water level: 25% LED is solid ON
    digitalWrite(LED_25_PIN, (currentWaterLevel >= 25) ? HIGH : LOW);
    digitalWrite(LED_50_PIN, (currentWaterLevel >= 50) ? HIGH : LOW);
    digitalWrite(LED_75_PIN, (currentWaterLevel >= 75) ? HIGH : LOW);
    digitalWrite(LED_100_PIN, (currentWaterLevel >= 100) ? HIGH : LOW);
  }
}

//==================================================
// MOTOR CONTROL ACTIONS (SG90 Servo on Pin D13)
//==================================================

void turnMotorON() {
  Serial.printf("\n>>> TURNING MOTOR ON >>> Rotating SG90 Servo to %d degrees\n", angleON);
  currentAngle = angleON;
  motorRunning = true;
  motorStatus  = "ON";

  // Persist motor state in flash memory
  preferences.putBool("running", true);
  preferences.putString("status", "ON");

  motorServo.write(angleON);

  // Immediately notify server after turning ON!
  sendMotorStatusToServer("motor turned ON (servo rotated to " + String(angleON) + " deg)");

  // Wait configured hold time (default 1000ms), then return servo back to default 90 degrees
  delay(holdDurationMs);
  Serial.printf(">>> %d MS ELAPSED >>> Returning SG90 Servo back to default 90 degrees\n", holdDurationMs);
  currentAngle = ANGLE_DEFAULT; // 90 degrees
  motorServo.write(ANGLE_DEFAULT);

  // Notify server that servo returned to 90 deg resting position while motor stays ON
  sendMotorStatusToServer("servo returned to 90 deg resting position (motor ON)");
}

void turnMotorOFF() {
  Serial.printf("\n>>> TURNING MOTOR OFF >>> Rotating SG90 Servo to %d degrees\n", angleOFF);
  currentAngle = angleOFF;
  motorRunning = false;
  motorStatus  = "OFF";

  // Persist motor state in flash memory
  preferences.putBool("running", false);
  preferences.putString("status", "OFF");

  motorServo.write(angleOFF);

  // Immediately notify server after turning OFF!
  sendMotorStatusToServer("motor turned OFF (servo rotated to " + String(angleOFF) + " deg)");

  // Wait configured hold time (default 1000ms), then return servo back to default 90 degrees
  delay(holdDurationMs);
  Serial.printf(">>> %d MS ELAPSED >>> Returning SG90 Servo back to default 90 degrees\n", holdDurationMs);
  currentAngle = ANGLE_DEFAULT; // 90 degrees
  motorServo.write(ANGLE_DEFAULT);

  // Notify server that servo returned to 90 deg resting position while motor stays OFF
  sendMotorStatusToServer("servo returned to 90 deg resting position (motor OFF)");
}

void processCommand(const String &cmd) {
  if (cmd.startsWith("SET_HOLD_TIME:") || cmd.startsWith("HOLD_MS:")) {
    int val = cmd.substring(cmd.indexOf(':') + 1).toInt();
    if (val >= 100 && val <= 30000) {
      holdDurationMs = val;
      preferences.putInt("holdMs", holdDurationMs);
      Serial.printf("[esp32motor] Updated Return to Default Hold Time: %d ms\n", holdDurationMs);
      sendMotorStatusToServer("updated return delay to " + String(holdDurationMs) + " ms");
    }
  } else if (cmd.startsWith("SET_HOLD_SEC:")) {
    float sec = cmd.substring(13).toFloat();
    int val = (int)(sec * 1000.0);
    if (val >= 100 && val <= 30000) {
      holdDurationMs = val;
      preferences.putInt("holdMs", holdDurationMs);
      Serial.printf("[esp32motor] Updated Return to Default Hold Time: %d ms\n", holdDurationMs);
      sendMotorStatusToServer("updated return delay to " + String(holdDurationMs) + " ms");
    }
  } else if (cmd.startsWith("SET_ON_ANGLE:") || cmd.startsWith("ANGLE_ON:")) {
    int val = cmd.substring(cmd.indexOf(':') + 1).toInt();
    if (val >= 0 && val <= 180) {
      angleON = val;
      preferences.putInt("angleON", angleON);
      Serial.printf("[esp32motor] Updated Start (ON) Angle to: %d deg\n", angleON);
      sendMotorStatusToServer("updated motor start angle to " + String(angleON) + " deg");
    }
  } else if (cmd.startsWith("SET_OFF_ANGLE:") || cmd.startsWith("ANGLE_OFF:")) {
    int val = cmd.substring(cmd.indexOf(':') + 1).toInt();
    if (val >= 0 && val <= 180) {
      angleOFF = val;
      preferences.putInt("angleOFF", angleOFF);
      Serial.printf("[esp32motor] Updated Stop (OFF) Angle to: %d deg\n", angleOFF);
      sendMotorStatusToServer("updated motor stop angle to " + String(angleOFF) + " deg");
    }
  } else if (cmd.startsWith("SET_ANGLES:")) {
    String rest = cmd.substring(11);
    int comma = rest.indexOf(',');
    if (comma > 0) {
      int onVal = rest.substring(0, comma).toInt();
      int offVal = rest.substring(comma + 1).toInt();
      if (onVal >= 0 && onVal <= 180) { angleON = onVal; preferences.putInt("angleON", angleON); }
      if (offVal >= 0 && offVal <= 180) { angleOFF = offVal; preferences.putInt("angleOFF", angleOFF); }
      Serial.printf("[esp32motor] Updated angles: ON=%d deg, OFF=%d deg\n", angleON, angleOFF);
      sendMotorStatusToServer("updated motor angles: ON=" + String(angleON) + " deg, OFF=" + String(angleOFF) + " deg");
    }
  } else if (cmd == "MOTOR_ON" || cmd == "ON" || cmd == "TURN_ON" || cmd == "START" || cmd.startsWith("MOTOR_ON:")) {
    if (cmd.startsWith("MOTOR_ON:")) {
      int val = cmd.substring(9).toInt();
      if (val >= 0 && val <= 180) { angleON = val; preferences.putInt("angleON", angleON); }
    }
    turnMotorON();
  } else if (cmd == "MOTOR_OFF" || cmd == "OFF" || cmd == "TURN_OFF" || cmd == "STOP" || cmd.startsWith("MOTOR_OFF:")) {
    if (cmd.startsWith("MOTOR_OFF:")) {
      int val = cmd.substring(10).toInt();
      if (val >= 0 && val <= 180) { angleOFF = val; preferences.putInt("angleOFF", angleOFF); }
    }
    turnMotorOFF();
  }
}

//==================================================
// SETUP
//==================================================
void setup() {
  // Disable brownout detector to prevent ESP32 reset during servo current spikes
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("==================================================");
  Serial.println("ESP32 MOTOR PROGRAM (SG90 Servo @ Pin D13 + 4 Water Level LEDs)");
  Serial.println("==================================================");

  // Initialize NVS Preferences storage to persist motor state, angles & hold delay across reboots
  preferences.begin("esp32motor", false);
  motorRunning   = preferences.getBool("running", false);
  motorStatus    = preferences.getString("status", motorRunning ? "ON" : "OFF");
  angleON        = preferences.getInt("angleON", 45);
  angleOFF       = preferences.getInt("angleOFF", 135);
  holdDurationMs = preferences.getInt("holdMs", 1000);

  // Configure LED pins as outputs
  pinMode(LED_25_PIN, OUTPUT);
  pinMode(LED_50_PIN, OUTPUT);
  pinMode(LED_75_PIN, OUTPUT);
  pinMode(LED_100_PIN, OUTPUT);

  digitalWrite(LED_25_PIN, LOW);
  digitalWrite(LED_50_PIN, LOW);
  digitalWrite(LED_75_PIN, LOW);
  digitalWrite(LED_100_PIN, LOW);

  // CAREFULLY SET DEFAULT ANGLE EVEN AT STARTUP TO 90 DEGREES
  motorServo.setPeriodHertz(50);             // Standard 50Hz servo PWM frequency
  motorServo.write(ANGLE_DEFAULT);           // Pre-set 90 deg position before attach
  motorServo.attach(SERVO_PIN, 500, 2400);   // Attach SG90 Servo to GPIO 13 (D13)
  motorServo.write(ANGLE_DEFAULT);           // Ensure angle is firmly 90 degrees at startup

  currentAngle = ANGLE_DEFAULT; // 90 degrees

  Serial.printf("Servo attached to GPIO 13 (D13). Startup default angle: %d deg\n", currentAngle);
  Serial.printf("Configured Angles: ON=%d deg, OFF=%d deg | Hold Delay=%d ms\n", angleON, angleOFF, holdDurationMs);
  Serial.printf("Restored Motor State: Running=%s, Status=%s\n", motorRunning ? "true" : "false", motorStatus.c_str());
  Serial.printf("LED Pins configured: 25%%=D25, 50%%=D26, 75%%=D27, 100%%=D14\n");

  // Connect to WiFi network
  if (connectWiFi()) {
    discoverServer();
    // Notify server of startup status & default 90 deg angle
    sendMotorStatusToServer("esp32motor booted - state, angles & hold delay restored from flash");
  }
}

//==================================================
// MAIN LOOP
//==================================================
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  // Continuously update LED display (0.5s blink on 25% LED when empty)
  updateWaterLevelLEDs();

  // Sync telemetry and poll server commands every 2 seconds
  unsigned long now = millis();
  if (now - lastSyncTime >= 2000) {
    lastSyncTime = now;
    sendMotorStatusToServer();
  }

  delay(10);
}


