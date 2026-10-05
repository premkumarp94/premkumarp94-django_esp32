#include <ESP32Servo.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_now.h>
#include <esp_wifi.h>
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
int currentWaterLevel = 0; // Synced water level from Django server or ESP-NOW
uint32_t syncIntervalSeconds = 2; // Server synced interval

// Loop timer tracking
unsigned long lastSyncTime = 0;

//==================================================
// ESP-NOW (WIFINOW) P2P DATA STRUCTURE & LOGIC
//==================================================

// ESP-NOW Data Structure (Shared between ESP8266 Tank & ESP32 Motor)
typedef struct __attribute__((packed)) {
  char device_id[32];     // "esp8266_device_01"
  int water_level;        // 0, 25, 50, 75, 100
  bool probe_25;
  bool probe_50;
  bool probe_75;
  bool probe_100;
  bool motor_running;     // Reported or requested motor status
  uint32_t interval_sec;  // Synced reading interval
  uint32_t msg_count;     // Packet sequence counter
} EspNowMessage;

bool espNowInitialized = false;
uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Forward declarations
void turnMotorOFF();
void turnMotorON();
void processCommand(const String &cmd);
void updateWaterLevelLEDs();

#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 5
void onEspNowRecv32(const esp_now_recv_info_t *recv_info, const uint8_t *incomingData, int len) {
  const uint8_t *mac = recv_info->src_addr;
#else
void onEspNowRecv32(const uint8_t *mac, const uint8_t *incomingData, int len) {
#endif
  if (len == sizeof(EspNowMessage)) {
    EspNowMessage msg;
    memcpy(&msg, incomingData, sizeof(msg));

    Serial.printf("\n[ESP-NOW RX] Data from %s: Water Level=%d%% (Probe 100: %s)\n",
                  msg.device_id, msg.water_level, msg.probe_100 ? "YES" : "NO");

    // Update local water level state immediately
    currentWaterLevel = msg.water_level;
    updateWaterLevelLEDs();

    // AUTO-SHUTOFF FAILSAFE VIA ESP-NOW:
    // If water level reaches 100% or probe 100 is detected, AND motor is running -> TURN OFF MOTOR
    if ((msg.water_level >= 100 || msg.probe_100) && motorRunning) {
      Serial.println("\n[ESP-NOW AUTO-SHUTOFF] Tank 100% FULL detected via ESP-NOW! Turning OFF motor immediately...");
      turnMotorOFF();
    }
  }
}

void initEspNow32() {
  if (espNowInitialized) return;

  WiFi.mode(WIFI_STA);

  if (esp_now_init() == ESP_OK) {
    espNowInitialized = true;
    esp_now_register_recv_cb(onEspNowRecv32);

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, broadcastMac, 6);
    peerInfo.channel = 0; // Current WiFi channel
    peerInfo.encrypt = false;

    if (!esp_now_is_peer_exist(broadcastMac)) {
      esp_now_add_peer(&peerInfo);
    }

    Serial.println("[ESP-NOW] Initialized successfully on ESP32 Motor Controller.");
  } else {
    Serial.println("[ESP-NOW] Initialization failed on ESP32.");
  }
}

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
  while (WiFi.status() != WL_CONNECTED && attempts < 15) {
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

  Serial.println("[esp32motor] WiFi Connection Failed (Offline ESP-NOW mode active).");
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

  // Parse server response for water level, interval & queued motor commands
  StaticJsonDocument<512> respDoc;
  DeserializationError err = deserializeJson(respDoc, response);
  if (!err) {
    if (respDoc.containsKey("water_level") && !respDoc["water_level"].isNull()) {
      currentWaterLevel = respDoc["water_level"].as<int>();
      Serial.printf("[esp32motor] Synced live water level from server: %d%%\n", currentWaterLevel);

      // Server check & Local failsafe: auto turn off motor if water level hits 100% and motor is running
      if (currentWaterLevel >= 100 && motorRunning) {
        Serial.println("[esp32motor AUTO-SHUTOFF] Tank is 100% FULL! Turning OFF motor...");
        turnMotorOFF();
      }
    }

    // Sync interval setting from server response
    if (respDoc.containsKey("interval_seconds")) {
      uint32_t sSec = respDoc["interval_seconds"].as<uint32_t>();
      if (sSec >= 1 && sSec <= 86400) {
        syncIntervalSeconds = sSec;
        Serial.printf("[SERVER SYNC] Synced interval: %u sec\n", syncIntervalSeconds);
      }
    } else if (respDoc.containsKey("sync_interval")) {
      uint32_t sSec = respDoc["sync_interval"].as<uint32_t>();
      if (sSec >= 1 && sSec <= 86400) {
        syncIntervalSeconds = sSec;
        Serial.printf("[SERVER SYNC] Synced interval: %u sec\n", syncIntervalSeconds);
      }
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
  Serial.printf("\n>>> TURNING MOTOR ON >>> Rotating SG90 Servo to %d degrees for %d ms\n", angleON, holdDurationMs);
  currentAngle = angleON;
  motorRunning = true;
  motorStatus  = "ON";

  // Persist motor state in flash memory
  preferences.putBool("running", true);
  preferences.putString("status", "ON");

  // 1. Instantly rotate servo to START (ON) angle
  motorServo.write(angleON);

  // 2. Hold at START angle for EXACT configured duration (e.g. 500ms = 0.5s)
  delay(holdDurationMs);

  // 3. Immediately return servo back to default 90 degrees resting position
  currentAngle = ANGLE_DEFAULT; // 90 degrees
  motorServo.write(ANGLE_DEFAULT);
  Serial.printf(">>> %d MS ELAPSED >>> SG90 Servo returned to default 90 degrees\n", holdDurationMs);

  // 4. Send telemetry sync to Django server AFTER physical servo sequence completes
  if (WiFi.status() == WL_CONNECTED) {
    sendMotorStatusToServer("motor turned ON (servo pulsed to " + String(angleON) + " deg for " + String(holdDurationMs) + "ms, returned to 90 deg)");
  }
}

void turnMotorOFF() {
  Serial.printf("\n>>> TURNING MOTOR OFF >>> Rotating SG90 Servo to %d degrees for %d ms\n", angleOFF, holdDurationMs);
  currentAngle = angleOFF;
  motorRunning = false;
  motorStatus  = "OFF";

  // Persist motor state in flash memory
  preferences.putBool("running", false);
  preferences.putString("status", "OFF");

  // 1. Instantly rotate servo to STOP (OFF) angle
  motorServo.write(angleOFF);

  // 2. Hold at STOP angle for EXACT configured duration (e.g. 500ms = 0.5s)
  delay(holdDurationMs);

  // 3. Immediately return servo back to default 90 degrees resting position
  currentAngle = ANGLE_DEFAULT; // 90 degrees
  motorServo.write(ANGLE_DEFAULT);
  Serial.printf(">>> %d MS ELAPSED >>> SG90 Servo returned to default 90 degrees\n", holdDurationMs);

  // 4. Send telemetry sync to Django server AFTER physical servo sequence completes
  if (WiFi.status() == WL_CONNECTED) {
    sendMotorStatusToServer("motor turned OFF (servo pulsed to " + String(angleOFF) + " deg for " + String(holdDurationMs) + "ms, returned to 90 deg)");
  }
}

void processCommand(const String &cmd) {
  int colonIndex = cmd.indexOf(':');

  if (cmd.startsWith("SET_HOLD_TIME:") || cmd.startsWith("HOLD_MS:") || cmd.startsWith("SET_HOLD_MS:")) {
    String valStr = (colonIndex != -1) ? cmd.substring(colonIndex + 1) : "";
    valStr.trim();
    int val = valStr.toInt();
    if (val >= 100 && val <= 30000) {
      holdDurationMs = val;
      preferences.putInt("holdMs", holdDurationMs);
      Serial.printf("[esp32motor] Updated Return Hold Delay: %d ms\n", holdDurationMs);
      sendMotorStatusToServer("updated return delay to " + String(holdDurationMs) + " ms");
    }
  } else if (cmd.startsWith("SET_HOLD_SEC:") || cmd.startsWith("HOLD_SEC:") || cmd.startsWith("HOLD_TIME:")) {
    String valStr = (colonIndex != -1) ? cmd.substring(colonIndex + 1) : "";
    valStr.trim();
    float sec = valStr.toFloat();
    int val = (int)(sec * 1000.0 + 0.5); // Accurately round float seconds to integer ms (e.g. 0.5s -> 500ms)
    if (val >= 100 && val <= 30000) {
      holdDurationMs = val;
      preferences.putInt("holdMs", holdDurationMs);
      Serial.printf("[esp32motor] Updated Return Hold Delay to: %d ms (%.2fs)\n", holdDurationMs, sec);
      sendMotorStatusToServer("updated return delay to " + String(holdDurationMs) + " ms");
    }
  } else if (cmd.startsWith("SET_ON_ANGLE:") || cmd.startsWith("ANGLE_ON:")) {
    String valStr = (colonIndex != -1) ? cmd.substring(colonIndex + 1) : "";
    valStr.trim();
    int val = valStr.toInt();
    if (val >= 0 && val <= 180) {
      angleON = val;
      preferences.putInt("angleON", angleON);
      Serial.printf("[esp32motor] Updated Start (ON) Angle to: %d deg\n", angleON);
      sendMotorStatusToServer("updated motor start angle to " + String(angleON) + " deg");
    }
  } else if (cmd.startsWith("SET_OFF_ANGLE:") || cmd.startsWith("ANGLE_OFF:")) {
    String valStr = (colonIndex != -1) ? cmd.substring(colonIndex + 1) : "";
    valStr.trim();
    int val = valStr.toInt();
    if (val >= 0 && val <= 180) {
      angleOFF = val;
      preferences.putInt("angleOFF", angleOFF);
      Serial.printf("[esp32motor] Updated Stop (OFF) Angle to: %d deg\n", angleOFF);
      sendMotorStatusToServer("updated motor stop angle to " + String(angleOFF) + " deg");
    }
  } else if (cmd.startsWith("SET_ANGLES:")) {
    String rest = (colonIndex != -1) ? cmd.substring(colonIndex + 1) : "";
    rest.trim();
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
      String valStr = (colonIndex != -1) ? cmd.substring(colonIndex + 1) : "";
      valStr.trim();
      int val = valStr.toInt();
      if (val >= 0 && val <= 180) { angleON = val; preferences.putInt("angleON", angleON); }
    }
    turnMotorON();
  } else if (cmd == "MOTOR_OFF" || cmd == "OFF" || cmd == "TURN_OFF" || cmd == "STOP" || cmd.startsWith("MOTOR_OFF:")) {
    if (cmd.startsWith("MOTOR_OFF:")) {
      String valStr = (colonIndex != -1) ? cmd.substring(colonIndex + 1) : "";
      valStr.trim();
      int val = valStr.toInt();
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
  Serial.println("ESP32 MOTOR PROGRAM (SG90 Servo @ Pin D13 + ESP-NOW + 4 LEDs)");
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

  // Initialize ESP-NOW peer-to-peer radio communication
  initEspNow32();

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
  bool isOnline = (WiFi.status() == WL_CONNECTED);

  // Continuously update LED display (0.5s blink on 25% LED when empty)
  updateWaterLevelLEDs();

  // Sync telemetry and poll server commands every 2 seconds
  unsigned long now = millis();
  if (now - lastSyncTime >= 2000) {
    lastSyncTime = now;
    if (isOnline) {
      sendMotorStatusToServer();
    } else {
      Serial.println("[OFFLINE MODE] Internet/Router unavailable. Listening for ESP-NOW radio signals from ESP8266 Tank sensor...");
    }
  }

  delay(10);
}
