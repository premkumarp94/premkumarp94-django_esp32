#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <espnow.h>

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

// Synced probe reading interval setting maintained from browser (Default: 60 seconds)
uint32_t readingIntervalSeconds = 60;

// Hardcoded online telemetry sync interval (Strictly 10 seconds)
const unsigned long ONLINE_SYNC_INTERVAL_MS = 10000;

// Non-blocking timer tracking variables
unsigned long lastProbeReadTime = 0;
unsigned long lastOnlineSyncTime = 0;

// Cached water level state
int cachedWaterLevel = 0;

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
bool motorRunning = false;

// Forward declarations
int readWaterLevel();
void sendEspNowTelemetry(int waterLevel);

void initEspNow() {
  if (espNowInitialized) return;

  WiFi.mode(WIFI_STA);

  if (esp_now_init() == 0) {
    espNowInitialized = true;
    esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
    esp_now_add_peer(broadcastMac, ESP_NOW_ROLE_COMBO, 1, NULL, 0);

    esp_now_register_recv_cb([](uint8_t *mac, uint8_t *incomingData, uint8_t len) {
      if (len == sizeof(EspNowMessage)) {
        EspNowMessage msg;
        memcpy(&msg, incomingData, sizeof(msg));
        Serial.printf("[ESP-NOW RX] Message from %s: Motor Running=%s, Interval=%u\n",
                      msg.device_id, msg.motor_running ? "YES" : "NO", msg.interval_sec);

        bool motorChanged = (msg.motor_running != motorRunning);
        bool intervalChanged = (msg.interval_sec >= 1 && msg.interval_sec <= 86400 && msg.interval_sec != readingIntervalSeconds);

        motorRunning = msg.motor_running;
        if (msg.interval_sec >= 1 && msg.interval_sec <= 86400) {
          readingIntervalSeconds = msg.interval_sec;
        }

        if (motorChanged || intervalChanged) {
          Serial.println("[ESP-NOW RX] Motor status or Probe interval changed! Cancelling current delay & sampling probes immediately.");
          lastProbeReadTime = millis();
          cachedWaterLevel = readWaterLevel();
          sendEspNowTelemetry(cachedWaterLevel);
        }
      }
    });
    Serial.println("[ESP-NOW] Initialized successfully on ESP8266.");
  } else {
    Serial.println("[ESP-NOW] Initialization failed on ESP8266.");
  }
}

//==================================================
// WATER PROBES & WIRE COLORS
//==================================================
// RED    = COM (Reference probe)
// BLACK  = D1  (100% Probe)
// GREEN  = D5  (75% Probe)
// YELLOW = D2  (50% Probe)
// BLUE   = D6  (25% Probe)

const int PROBE_25  = D6; // Blue
const int PROBE_50  = D2; // Yellow
const int PROBE_75  = D5; // Green
const int PROBE_100 = D1; // Black

// Using ESP8266 internal pull-up resistors.
// Probe in air   = HIGH
// Probe in water = LOW
const bool USE_INTERNAL_PULLUP = true;

// Individual probe detection states
bool probe25Detected = false;
bool probe50Detected = false;
bool probe75Detected = false;
bool probe100Detected = false;

int lastWaterLevel = -1;

void sendEspNowTelemetry(int waterLevel) {
  initEspNow();

  EspNowMessage msg;
  memset(&msg, 0, sizeof(msg));
  strncpy(msg.device_id, DEVICE_ID, sizeof(msg.device_id) - 1);
  msg.water_level = waterLevel;
  msg.probe_25 = probe25Detected;
  msg.probe_50 = probe50Detected;
  msg.probe_75 = probe75Detected;
  msg.probe_100 = probe100Detected;
  msg.motor_running = motorRunning;
  msg.interval_sec = readingIntervalSeconds;
  static uint32_t seq = 0;
  msg.msg_count = ++seq;

  int result = esp_now_send(broadcastMac, (uint8_t *)&msg, sizeof(msg));
  if (result == 0) {
    Serial.printf("[ESP-NOW TX] Broadcasted Water Level %d%% (Probe 100: %s, Auto OFF: %s) via ESP-NOW\n",
                  waterLevel, probe100Detected ? "YES" : "NO", (waterLevel >= 100) ? "YES" : "NO");
  } else {
    Serial.printf("[ESP-NOW TX ERROR] Send failed with code: %d\n", result);
  }
}

//==================================================
// SERVERS
//==================================================

const char *serverList[] = {
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
  Serial.printf("[WIFI CHECK] Primary Home WiFi (%s)... Attempting connection...\n", ssid);

  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 10) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WIFI CONNECTED] Primary Home WiFi Active! IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.println("[OFFLINE MODE] Home WiFi / Internet unavailable. Operating in ESP-NOW local radio mode.");
  return false;
}

//==================================================
// WATER LEVEL READ
//==================================================

int readWaterLevel() {
  // Dynamic discharge delay based on water level:
  // 0%, 25%, 50% -> 20ms delay | 75%, 100% -> 50ms delay | default -> 30ms delay
  int dischargeDelay = 30;
  if (lastWaterLevel == 0 || lastWaterLevel == 25 || lastWaterLevel == 50) {
    dischargeDelay = 20;
  } else if (lastWaterLevel == 75 || lastWaterLevel == 100) {
    dischargeDelay = 50;
  }

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
  delay(dischargeDelay);

  // 3. Read probe states (In water: charge bleeds to GND -> pin reads LOW. In air: stays HIGH)
  probe25Detected = (digitalRead(PROBE_25) == LOW);
  probe50Detected = (digitalRead(PROBE_50) == LOW);
  probe75Detected = (digitalRead(PROBE_75) == LOW);
  probe100Detected = (digitalRead(PROBE_100) == LOW);

  Serial.println();
  Serial.println("===== WATER PROBE TEST =====");
  Serial.print("Discharge Delay Used: ");
  Serial.print(dischargeDelay);
  Serial.println(" ms");

  Serial.print("25%  (D6 / BLUE)   = ");
  Serial.println(probe25Detected ? "DETECTED (WATER)" : "OPEN (AIR)");

  Serial.print("50%  (D2 / YELLOW) = ");
  Serial.println(probe50Detected ? "DETECTED (WATER)" : "OPEN (AIR)");

  Serial.print("75%  (D5 / GREEN)  = ");
  Serial.println(probe75Detected ? "DETECTED (WATER)" : "OPEN (AIR)");

  Serial.print("100% (D1 / BLACK)  = ");
  Serial.println(probe100Detected ? "DETECTED (WATER)" : "OPEN (AIR)");

  // Highest detected probe determines the level.
  int currentLevel = 0;
  if (probe100Detected) {
    currentLevel = 100;
  } else if (probe75Detected) {
    currentLevel = 75;
  } else if (probe50Detected) {
    currentLevel = 50;
  } else if (probe25Detected) {
    currentLevel = 25;
  } else {
    currentLevel = 0;
  }

  lastWaterLevel = currentLevel;
  return currentLevel;
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

String buildTelemetry(const String &ack, const String &message, int waterLevel) {
  StaticJsonDocument<512> doc;

  doc["id"] = DEVICE_ID;

  JsonObject sensor = doc.createNestedObject("sensor values");

  sensor["water_level"] = waterLevel;
  sensor["probe_25"] = probe25Detected;
  sensor["probe_50"] = probe50Detected;
  sensor["probe_75"] = probe75Detected;
  sensor["probe_100"] = probe100Detected;

  // Calculate elapsed seconds since physical water probes were sampled
  unsigned long probeAgeSec = 0;
  if (lastProbeReadTime > 0 && millis() >= lastProbeReadTime) {
    probeAgeSec = (millis() - lastProbeReadTime) / 1000;
  }
  sensor["probe_read_ago_sec"] = probeAgeSec;

  doc["ack"] = ack;

  doc["message"] = message;

  String body;

  serializeJson(doc, body);

  return body;
}

//==================================================
// SEND NORMAL TELEMETRY (ONLINE HARDCODED 10s TIMER)
//==================================================

bool sendTelemetry(int waterLevel) {
  if (activeServer == -1) {
    if (!discoverServer()) {
      return false;
    }
  }

  String ack = "dummy_ack";

  String message = pendingDeviceMsg;

  String body = buildTelemetry(ack, message, waterLevel);

  Serial.println();
  Serial.println("Sending online telemetry (10s Hardcoded Sync Timer):");

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

  // Parse server response to sync motor_running state and reading interval
  StaticJsonDocument<512> respDoc;
  DeserializationError err = deserializeJson(respDoc, response);
  if (!err) {
    bool newMotorRunning = motorRunning;
    if (respDoc.containsKey("motor_running")) {
      newMotorRunning = respDoc["motor_running"].as<bool>();
    } else if (respDoc.containsKey("is_filling")) {
      newMotorRunning = respDoc["is_filling"].as<bool>();
    }

    uint32_t newIntervalSec = readingIntervalSeconds;

    // Sync reading interval maintained from browser setting
    if (respDoc.containsKey("interval_seconds")) {
      uint32_t sSec = respDoc["interval_seconds"].as<uint32_t>();
      if (sSec >= 1 && sSec <= 86400) {
        newIntervalSec = sSec;
      }
    } else if (respDoc.containsKey("sync_interval")) {
      uint32_t sSec = respDoc["sync_interval"].as<uint32_t>();
      if (sSec >= 1 && sSec <= 86400) {
        newIntervalSec = sSec;
      }
    } else if (respDoc.containsKey("sleep_seconds")) {
      uint32_t sSec = respDoc["sleep_seconds"].as<uint32_t>();
      if (sSec >= 1 && sSec <= 86400) {
        newIntervalSec = sSec;
      }
    }

    // Sync interval from server commands (e.g. INTERVAL:30 or SLEEP:60)
    if (respDoc.containsKey("command")) {
      String cmd = respDoc["command"].as<String>();
      cmd.trim();
      cmd.toUpperCase();
      if (cmd.startsWith("INTERVAL:") || cmd.startsWith("SLEEP:") || cmd.startsWith("DEEPSLEEP:")) {
        int val = cmd.substring(cmd.indexOf(':') + 1).toInt();
        if (val >= 1 && val <= 86400) {
          newIntervalSec = (uint32_t)val;
        }
      }
    }

    bool motorChanged = (newMotorRunning != motorRunning);
    bool intervalChanged = (newIntervalSec != readingIntervalSeconds);

    motorRunning = newMotorRunning;
    readingIntervalSeconds = newIntervalSec;

    if (motorChanged || intervalChanged) {
      Serial.printf("[HTTP SYNC] Motor status (%s) or Probe interval (%u sec) changed! Cancelling current delay & sampling probes immediately.\n",
                    motorRunning ? "ON" : "OFF", readingIntervalSeconds);
      lastProbeReadTime = millis();
      cachedWaterLevel = readWaterLevel();
      sendEspNowTelemetry(cachedWaterLevel);
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

  // Initial water probe reading at boot
  cachedWaterLevel = readWaterLevel();

  //================================================
  // ESP-NOW (WIFINOW) INITIALIZATION
  //================================================

  initEspNow();

  //================================================
  // WIFI & SERVER CONNECT
  //================================================

  if (connectWiFi()) {
    discoverServer();
  }
}

//==================================================
// LOOP
//==================================================

void loop() {
  unsigned long now = millis();

  //================================================
  // 1. SEPARATE PROBE READING & ESP-NOW TIMER
  // Maintained from browser-synced readingIntervalSeconds (or 10s if motor running)
  //================================================
  uint32_t activeReadingIntervalSec = motorRunning ? 10 : readingIntervalSeconds;
  unsigned long probeReadIntervalMs = (unsigned long)activeReadingIntervalSec * 1000;

  if (now - lastProbeReadTime >= probeReadIntervalMs || lastProbeReadTime == 0) {
    lastProbeReadTime = now;

    // Read water level probes locally
    cachedWaterLevel = readWaterLevel();

    // Broadcast probe state & water level via ESP-NOW (wifinow)
    // Ensures ESP32 Motor Controller receives 100% auto-shutoff immediately, online or offline!
    sendEspNowTelemetry(cachedWaterLevel);

    Serial.printf("\n[PROBE TIMER] Water Level sampled: %d%% | Next probe sample in %u sec (Browser Setting: %u sec)\n",
                  cachedWaterLevel, activeReadingIntervalSec, readingIntervalSeconds);
  }

  //================================================
  // 2. SEPARATE HARDCODED 10-SECOND ONLINE TELEMETRY SYNC TIMER
  // Strictly syncs online HTTP telemetry every 10 seconds
  //================================================
  if (now - lastOnlineSyncTime >= ONLINE_SYNC_INTERVAL_MS || lastOnlineSyncTime == 0) {
    lastOnlineSyncTime = now;

    bool isOnline = connectWiFi();
    if (isOnline) {
      bool httpSuccess = sendTelemetry(cachedWaterLevel);
      if (!httpSuccess) {
        Serial.println("[INTERNET CHECK] Home WiFi connected, but HTTP server ping failed. Operating in ESP-NOW local mode until internet returns.");
      }
    } else {
      Serial.println("[OFFLINE MODE] Home WiFi / Internet unavailable. Operating in ESP-NOW local radio mode with ESP32 Motor.");
    }

    if (iterationCount == 0) {
      pendingDeviceMsg = "came to first iteration";
    }

    iterationCount++;

    Serial.println();
    Serial.printf("Iteration: %d | Mode: %s (10s Hardcoded Online Timer) | Motor Status: ",
                  iterationCount, isOnline ? "ONLINE" : "OFFLINE");
    if (motorRunning) {
      Serial.println("RUNNING (Motor is ON -> Probe reading interval forced to 10s)");
    } else {
      Serial.printf("OFF / IDLE (Motor is OFF -> Browser Synced Probe Interval: %u sec)\n", readingIntervalSeconds);
    }

    Serial.println("------------------------------------");
  }

  delay(50); // Non-blocking loop iteration delay
}