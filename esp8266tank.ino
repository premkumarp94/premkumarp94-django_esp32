#include <ESP8266WiFi.h>
#include <espnow.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

//==================================================
// WIFI CREDENTIALS (ESP8266 Home Gateway)
//==================================================
const char *ssid     = "TIC_5G-PREM";
const char *password = "prem@123";

// Deep Sleep interval to relay to ESP32 Tank Sensor (Default 5s)
uint32_t currentSleepSeconds = 5;

// Server list (Local LAN Django Server first priority, then Cloud fallback)
const char *serverList[] = {
    "https://premkumarp94.pythonanywhere.com/api/telemetry/",
    "http://192.168.1.7:8000/api/telemetry/", // Local PC IP on Home WiFi
    "http://192.168.1.2:8000/api/telemetry/",
    "http://192.168.1.3:8000/api/telemetry/",
    "http://192.168.1.4:8000/api/telemetry/",
    "http://192.168.1.5:8000/api/telemetry/",
    "http://192.168.1.6:8000/api/telemetry/"
};
const int SERVER_COUNT = sizeof(serverList) / sizeof(serverList[0]);

// Currently locked active server index (Persisted across requests, defaults to 0)
int activeServerIndex = 0;

//==================================================
// TELEMETRY & CONFIG DATA STRUCTURES (Packed)
//==================================================
typedef struct __attribute__((packed)) struct_telemetry {
  char device_id[32];
  int water_level;
  bool probe_25;
  bool probe_50;
  bool probe_75;
  bool probe_100;
  uint32_t message_count;
} TelemetryData;

typedef struct __attribute__((packed)) struct_sleep_config {
  uint32_t sleep_seconds;
} SleepConfigData;

TelemetryData latestTelemetry;
uint8_t senderMac[6];
volatile bool newTelemetryAvailable = false;
uint32_t processedCount = 0;
bool motorRunning = false;

//==================================================
// LED LEVEL INDICATORS & BLINK LOGIC
// D1 = GPIO 5  -> 25% LED  (LED 1 - Blinks once/sec when 0%)
// D2 = GPIO 4  -> 50% LED  (LED 2)
// D5 = GPIO 14 -> 75% LED  (LED 3)
// D6 = GPIO 12 -> 100% LED (LED 4)
//==================================================
const int LED_25  = D1;
const int LED_50  = D2;
const int LED_75  = D5;
const int LED_100 = D6;

unsigned long lastBlinkTime = 0;
bool blinkState = false;

void updateLEDDisplay() {
  bool led1 = latestTelemetry.probe_25  || (latestTelemetry.water_level >= 25);
  bool led2 = latestTelemetry.probe_50  || (latestTelemetry.water_level >= 50);
  bool led3 = latestTelemetry.probe_75  || (latestTelemetry.water_level >= 75);
  bool led4 = latestTelemetry.probe_100 || (latestTelemetry.water_level >= 100);

  // Set LEDs 2, 3, 4 solid ON/OFF according to percentage
  digitalWrite(LED_50,  led2 ? HIGH : LOW);
  digitalWrite(LED_75,  led3 ? HIGH : LOW);
  digitalWrite(LED_100, led4 ? HIGH : LOW);

  if (led1) {
    // Water level >= 25%: LED 1 is SOLID ON
    digitalWrite(LED_25, HIGH);
  } else {
    // Water level is 0%: Blink LED 1 (D1) once per second (500ms ON / 500ms OFF)
    unsigned long now = millis();
    if (now - lastBlinkTime >= 500) {
      lastBlinkTime = now;
      blinkState = !blinkState;
      digitalWrite(LED_25, blinkState ? HIGH : LOW);
    }
  }
}

//==================================================
// PARSE DJANGO RESPONSE COMMANDS
//==================================================
void processServerCommands(const String &response) {
  StaticJsonDocument<512> respDoc;
  DeserializationError err = deserializeJson(respDoc, response);
  if (!err) {
    if (respDoc.containsKey("motor_running")) {
      motorRunning = respDoc["motor_running"].as<bool>();
    }
    if (respDoc.containsKey("sleep_seconds")) {
      currentSleepSeconds = respDoc["sleep_seconds"].as<uint32_t>();
      Serial.printf("[DJANGO CMD] New Deep Sleep duration from server: %u sec\n", currentSleepSeconds);
    }
    if (respDoc.containsKey("command")) {
      String cmd = respDoc["command"].as<String>();
      if (cmd.startsWith("SLEEP:") || cmd.startsWith("DEEPSLEEP:")) {
        int val = cmd.substring(cmd.indexOf(':') + 1).toInt();
        if (val >= 1 && val <= 86400) {
          currentSleepSeconds = (uint32_t)val;
          Serial.printf("[DJANGO CMD] Received command update: Deep Sleep = %u sec\n", currentSleepSeconds);
        }
      }
    }
  }
}

//==================================================
// ESP-NOW RECEIVE CALLBACK (ESP8266 Gateway)
//==================================================
void OnDataRecv(uint8_t *mac, uint8_t *incomingData, uint8_t len) {
  if (len == sizeof(TelemetryData)) {
    memcpy(&latestTelemetry, incomingData, sizeof(TelemetryData));
    memcpy(senderMac, mac, 6);
    newTelemetryAvailable = true;

    Serial.printf("\n[ESP-NOW RX] From ESP32 MAC: %02X:%02X:%02X:%02X:%02X:%02X | Level: %d%% | Msg #%u\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  latestTelemetry.water_level, latestTelemetry.message_count);

    // Send immediate ACK back to ESP32 with current Deep Sleep setting
    SleepConfigData reply;
    reply.sleep_seconds = currentSleepSeconds;

    uint8_t currentChannel = WiFi.channel();
    if (currentChannel == 0) currentChannel = 9;

    if (!esp_now_is_peer_exist(mac)) {
      esp_now_add_peer(mac, ESP_NOW_ROLE_COMBO, currentChannel, NULL, 0);
    }
    int result = esp_now_send(mac, (uint8_t *)&reply, sizeof(reply));
    if (result == 0) {
      Serial.printf("[ESP-NOW TX ACK] Sent Deep Sleep duration (%u sec) to ESP32\n", currentSleepSeconds);
    } else {
      Serial.printf("[ESP-NOW TX ACK ERROR] Code: %d\n", result);
    }
  }
}

//==================================================
// CONNECT TO HOME WIFI
//==================================================
bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.println();
  Serial.print("Connecting ESP8266 Gateway to Home WiFi (SSID: ");
  Serial.print(ssid);
  Serial.println(")...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi Connected! ESP8266 Gateway Local IP Address: ");
    Serial.println(WiFi.localIP());
    Serial.printf("Active WiFi Channel: %d\n", WiFi.channel());
    return true;
  }

  Serial.println("WiFi connection failed! Unable to connect to Home WiFi AP.");
  return false;
}

//==================================================
// HTTP POST TO DJANGO SERVER
//==================================================
bool postJsonToCloud(const char *url, const String &body, String &responseOut) {
  HTTPClient http;
  int code = -1;

  Serial.print("[HTTP POST] Connecting to Target Server IP/URL: ");
  Serial.println(url);

  if (strncmp(url, "https://", 8) == 0) {
    WiFiClientSecure client;
    client.setInsecure();
    client.setBufferSizes(1024, 1024); // Optimize SSL handshake RAM for ESP8266

    if (http.begin(client, url)) {
      http.addHeader("Content-Type", "application/json");
      http.setTimeout(7000);
      code = http.POST(body);
      if (code > 0) {
        responseOut = http.getString();
      } else {
        Serial.printf("[HTTP ERROR] POST failed to %s | Error: %s\n", url, http.errorToString(code).c_str());
      }
      http.end();
    } else {
      Serial.printf("[HTTP ERROR] Unable to connect to HTTPS endpoint: %s\n", url);
    }
  } else {
    WiFiClient client;
    if (http.begin(client, url)) {
      http.addHeader("Content-Type", "application/json");
      http.setTimeout(5000);
      code = http.POST(body);
      if (code > 0) {
        responseOut = http.getString();
      } else {
        Serial.printf("[HTTP ERROR] POST failed to %s | Error: %s\n", url, http.errorToString(code).c_str());
      }
      http.end();
    } else {
      Serial.printf("[HTTP ERROR] Unable to connect to HTTP endpoint: %s\n", url);
    }
  }

  Serial.printf("[HTTP RESULT] Status Code: %d for Target IP/URL: %s\n", code, url);
  return (code >= 200 && code < 300);
}

//==================================================
// FORWARD TELEMETRY TO DJANGO CLOUD
//==================================================
bool forwardTelemetryToCloud(const TelemetryData &data) {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) return false;
  }

  StaticJsonDocument<512> doc;
  doc["id"] = data.device_id[0] != '\0' ? data.device_id : "esp32_device_01";

  JsonObject sensor = doc.createNestedObject("sensor values");
  sensor["water_level"] = data.water_level;
  sensor["probe_25"]    = data.probe_25;
  sensor["probe_50"]    = data.probe_50;
  sensor["probe_75"]    = data.probe_75;
  sensor["probe_100"]   = data.probe_100;

  doc["ack"] = "espnow_ok";
  doc["message"] = "esp32 telemetry via esp8266 gateway";

  String body;
  serializeJson(doc, body);

  Serial.println("\n--- Forwarding Telemetry to Django Cloud ---");
  Serial.print("Gateway Local IP: ");
  Serial.println(WiFi.localIP());
  Serial.println(body);

  String response;

  // STEP 1: Try current active server IP first
  Serial.printf("[ACTIVE TELEMETRY TARGET %d/%d] Sending to: %s\n", activeServerIndex + 1, SERVER_COUNT, serverList[activeServerIndex]);
  if (postJsonToCloud(serverList[activeServerIndex], body, response)) {
    Serial.printf("[HTTP SUCCESS] Uploaded telemetry via Active Server: %s\n", serverList[activeServerIndex]);
    Serial.println("Server Response:");
    Serial.println(response);

    processServerCommands(response);
    return true;
  }

  // STEP 2: Active server failed! Scan serverList from top to bottom
  Serial.printf("\n[ACTIVE SERVER FAILED] Could not reach %s!\n", serverList[activeServerIndex]);
  Serial.println("[FALLBACK SEARCH] Probing server list from top to bottom for a working server...");

  for (int i = 0; i < SERVER_COUNT; i++) {
    if (i == activeServerIndex) continue; // Already attempted above

    Serial.printf("[FALLBACK TRY %d/%d] Trying: %s ...\n", i + 1, SERVER_COUNT, serverList[i]);
    if (postJsonToCloud(serverList[i], body, response)) {
      activeServerIndex = i; // Save new active server!
      Serial.printf("=> ACTIVE SERVER UPDATED! Locked onto server [%d/%d]: %s\n", activeServerIndex + 1, SERVER_COUNT, serverList[activeServerIndex]);
      Serial.println("Server Response:");
      Serial.println(response);

      processServerCommands(response);
      return true;
    }
  }

  Serial.println("[ERROR] All servers in candidate list failed to respond!");
  return false;
}

//==================================================
// SETUP
//==================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  // Initialize LED Pins
  pinMode(LED_25, OUTPUT);
  pinMode(LED_50, OUTPUT);
  pinMode(LED_75, OUTPUT);
  pinMode(LED_100, OUTPUT);

  // LED startup test sweep
  digitalWrite(LED_25, HIGH); delay(150); digitalWrite(LED_25, LOW);
  digitalWrite(LED_50, HIGH); delay(150); digitalWrite(LED_50, LOW);
  digitalWrite(LED_75, HIGH); delay(150); digitalWrite(LED_75, LOW);
  digitalWrite(LED_100, HIGH); delay(150); digitalWrite(LED_100, LOW);

  Serial.println();
  Serial.println("==================================================");
  Serial.println("ESP8266 HOME WIFI GATEWAY & ESP-NOW RECEIVER");
  Serial.println("==================================================");

  // Connect to Home WiFi
  connectWiFi();

  Serial.print("ESP8266 MAC Address: ");
  Serial.println(WiFi.macAddress());

  // Print configured candidate server list at startup
  Serial.println("\n--------------------------------------------------");
  Serial.println("CONFIGURED DJANGO TELEMETRY SERVERS LIST:");
  for (int i = 0; i < SERVER_COUNT; i++) {
    Serial.printf("  Candidate [%d/%d]: %s\n", i + 1, SERVER_COUNT, serverList[i]);
  }
  Serial.println("--------------------------------------------------");

  // Try server list from top to bottom at startup, lock activeServerIndex to the first working server
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[STARTUP SERVER CHECK] Probing candidate servers from top to bottom...");
    String probeBody = "{\"id\":\"esp8266_gateway_boot\",\"message\":\"ping\"}";
    String probeResp;

    for (int i = 0; i < SERVER_COUNT; i++) {
      Serial.printf("\n[TRY %d/%d] Connecting to: %s ...\n", i + 1, SERVER_COUNT, serverList[i]);
      if (postJsonToCloud(serverList[i], probeBody, probeResp)) {
        activeServerIndex = i;
        Serial.printf("=> CONNECTED SUCCESS! Locked active telemetry server [%d/%d]: %s\n", activeServerIndex + 1, SERVER_COUNT, serverList[activeServerIndex]);
        Serial.println("=> Stopping startup search.\n");
        break; // Lock active server and stop search!
      } else {
        Serial.printf("=> FAILED to connect to %s. Trying next server in list...\n", serverList[i]);
      }
    }
  }

  // Initialize ESP-NOW
  if (esp_now_init() != 0) {
    Serial.println("Error initializing ESP-NOW!");
    return;
  }

  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(OnDataRecv);

  Serial.println("ESP8266 Gateway Ready. Listening for ESP32 Telemetry...");
}

//==================================================
// MAIN LOOP
//==================================================
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  // Continuously update LED display (blinks LED 1 once per second when water level is 0%)
  updateLEDDisplay();

  if (newTelemetryAvailable) {
    newTelemetryAvailable = false;
    processedCount++;

    Serial.printf("\n[Forwarding ESP32 Msg #%u to Cloud (Gateway Upload #%u)]\n", latestTelemetry.message_count, processedCount);
    forwardTelemetryToCloud(latestTelemetry);
  }

  delay(20);
}