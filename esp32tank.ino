#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

//==================================================
// DEVICE IDENTIFIER & CONFIG
//==================================================
const char *DEVICE_ID = "esp32_device_01";
const int WIFI_CHANNEL = 9; // Fixed Wi-Fi channel for ESP-NOW unicast

// Gateway ESP8266 MAC Address (Update with actual ESP8266 MAC if changed)
uint8_t gatewayMac[6] = { 0x50, 0x02, 0x91, 0xEF, 0x01, 0x8E };
uint8_t broadcastAddress[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// Persistent RTC RAM variables (Survives ESP32 Deep Sleep automatically)
RTC_DATA_ATTR uint32_t sleepDurationSeconds = 5; // Default 5 seconds
RTC_DATA_ATTR uint32_t messageCounter = 0;
volatile bool receivedConfig = false;

const int MAX_RETRIES = 4;

//==================================================
// WATER PROBES & PIN MAPPINGS (ESP32)
//==================================================
// RED    = COM (Reference probe -> Connect to GND!)
// BLACK  = GPIO 18 (100% Probe)
// GREEN  = GPIO 5  (75% Probe)
// YELLOW = GPIO 4  (50% Probe)
// BLUE   = GPIO 15 (25% Probe)

const int PROBE_25  = 15; // Blue (GPIO 15)
const int PROBE_50  = 4;  // Yellow (GPIO 4)
const int PROBE_75  = 5;  // Green (GPIO 5)
const int PROBE_100 = 18; // Black (GPIO 18)

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

TelemetryData telemetryPayload;

// Water level state
bool probe25Detected  = false;
bool probe50Detected  = false;
bool probe75Detected  = false;
bool probe100Detected = false;

//==================================================
// ESP-NOW SEND & RECEIVE CALLBACKS
//==================================================
#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 5
void OnDataSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
  Serial.print("[ESP-NOW TX STATUS] ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "SUCCESS" : "FAILED");
}

void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *incomingData, int len) {
  if (len == sizeof(SleepConfigData)) {
    SleepConfigData config;
    memcpy(&config, incomingData, sizeof(SleepConfigData));
    if (config.sleep_seconds >= 1 && config.sleep_seconds <= 86400) {
      sleepDurationSeconds = config.sleep_seconds;
      receivedConfig = true;
      Serial.printf("  [ESP-NOW ACK RECEIVED] Updated Deep Sleep: %u sec\n", sleepDurationSeconds);
    }
  }
}
#else
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("[ESP-NOW TX STATUS] ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "SUCCESS" : "FAILED");
}

void OnDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
  if (len == sizeof(SleepConfigData)) {
    SleepConfigData config;
    memcpy(&config, incomingData, sizeof(SleepConfigData));
    if (config.sleep_seconds >= 1 && config.sleep_seconds <= 86400) {
      sleepDurationSeconds = config.sleep_seconds;
      receivedConfig = true;
      Serial.printf("  [ESP-NOW ACK RECEIVED] Updated Deep Sleep: %u sec\n", sleepDurationSeconds);
    }
  }
}
#endif

//==================================================
// WATER LEVEL SENSING (INPUT_PULLUP)
// COM Probe (Red Wire) connected to GND
//==================================================
int readWaterLevel() {
  pinMode(PROBE_25, INPUT_PULLUP);
  pinMode(PROBE_50, INPUT_PULLUP);
  pinMode(PROBE_75, INPUT_PULLUP);
  pinMode(PROBE_100, INPUT_PULLUP);
  delay(20); // Allow pin states to settle

  probe25Detected  = (digitalRead(PROBE_25) == LOW);
  probe50Detected  = (digitalRead(PROBE_50) == LOW);
  probe75Detected  = (digitalRead(PROBE_75) == LOW);
  probe100Detected = (digitalRead(PROBE_100) == LOW);

  Serial.println();
  Serial.println("===== ESP32 WATER PROBE SENSING (INPUT_PULLUP) =====");
  Serial.printf("25%%  (GPIO 15/BLUE)   = %s\n", probe25Detected ? "WATER DETECTED (LOW)" : "AIR (HIGH)");
  Serial.printf("50%%  (GPIO 4/YELLOW)  = %s\n", probe50Detected ? "WATER DETECTED (LOW)" : "AIR (HIGH)");
  Serial.printf("75%%  (GPIO 5/GREEN)   = %s\n", probe75Detected ? "WATER DETECTED (LOW)" : "AIR (HIGH)");
  Serial.printf("100%% (GPIO 18/BLACK)  = %s\n", probe100Detected ? "WATER DETECTED (LOW)" : "AIR (HIGH)");

  int currentLevel = 0;
  if (probe100Detected)      currentLevel = 100;
  else if (probe75Detected) currentLevel = 75;
  else if (probe50Detected) currentLevel = 50;
  else if (probe25Detected) currentLevel = 25;
  else                       currentLevel = 0;

  Serial.printf("WATER LEVEL = %d%%\n", currentLevel);
  return currentLevel;
}

//==================================================
// SEND TELEMETRY VIA UNICAST ESP-NOW
//==================================================
bool sendEspNowTelemetry(uint8_t *targetMac) {
  strncpy(telemetryPayload.device_id, DEVICE_ID, sizeof(telemetryPayload.device_id));
  telemetryPayload.water_level   = readWaterLevel();
  telemetryPayload.probe_25      = probe25Detected;
  telemetryPayload.probe_50      = probe50Detected;
  telemetryPayload.probe_75      = probe75Detected;
  telemetryPayload.probe_100     = probe100Detected;
  telemetryPayload.message_count = messageCounter;

  esp_err_t err = esp_now_send(targetMac, (uint8_t *)&telemetryPayload, sizeof(telemetryPayload));
  return (err == ESP_OK);
}

//==================================================
// SETUP (Executes once per wake-up cycle)
//==================================================
void setup() {
  Serial.begin(115200);
  delay(200); // Allow UART to stabilize after sleep wake-up

  messageCounter++; // Increment sequence counter once per wake cycle

  Serial.println();
  Serial.println("==================================================");
  Serial.println("ESP32 WATER TANK SENSOR (DEEP SLEEP UNICAST MODE)");
  Serial.println("==================================================");
  Serial.printf("Device ID: %s | Sleep: %u sec | Msg #%u\n", DEVICE_ID, sleepDurationSeconds, messageCounter);

  // Setup Probe Pins
  pinMode(PROBE_25, INPUT_PULLUP);
  pinMode(PROBE_50, INPUT_PULLUP);
  pinMode(PROBE_75, INPUT_PULLUP);
  pinMode(PROBE_100, INPUT_PULLUP);

  // Initialize WiFi in Station Mode
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  // Set fixed Wi-Fi channel
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

  Serial.printf("ESP32 Wi-Fi Channel: %d\n", WIFI_CHANNEL);
  Serial.print("ESP32 MAC: ");
  Serial.println(WiFi.macAddress());

  // Initialize ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("[ERROR] Error initializing ESP-NOW!");
  } else {
    esp_now_register_send_cb(OnDataSent);
    esp_now_register_recv_cb(OnDataRecv);

    // Register Unicast Peer (Gateway MAC)
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, gatewayMac, 6);
    peerInfo.channel = WIFI_CHANNEL;
    peerInfo.encrypt = false;
    peerInfo.ifidx = WIFI_IF_STA;

    if (esp_now_is_peer_exist(gatewayMac)) {
      esp_now_del_peer(gatewayMac);
    }
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      Serial.println("[WARNING] Failed to register unicast peer, adding broadcast fallback...");
      memcpy(peerInfo.peer_addr, broadcastAddress, 6);
      peerInfo.channel = 0;
      if (!esp_now_is_peer_exist(broadcastAddress)) {
        esp_now_add_peer(&peerInfo);
      }
    }

    bool ackConfirmed = false;
    for (int attempt = 1; attempt <= MAX_RETRIES; attempt++) {
      receivedConfig = false;
      Serial.printf("\n[ESP-NOW TX] Attempt %d/%d for Msg #%u...\n", attempt, MAX_RETRIES, messageCounter);

      // Try unicast send first
      bool sendOk = sendEspNowTelemetry(gatewayMac);
      if (!sendOk) {
        // Fallback to broadcast across channels if unicast fails
        Serial.println("[ESP-NOW TX] Unicast send error. Sweeping channels with broadcast...");
        for (int ch = 1; ch <= 13; ch++) {
          esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
          sendEspNowTelemetry(broadcastAddress);
          delay(5);
        }
      }

      // Wait up to 250ms for ACK response from Gateway
      unsigned long startWait = millis();
      while (millis() - startWait < 250) {
        if (receivedConfig) {
          ackConfirmed = true;
          break;
        }
        delay(5);
      }

      if (ackConfirmed) {
        Serial.printf("[ESP-NOW SUCCESS] ACK confirmed on Attempt %d!\n", attempt);
        break;
      }

      if (attempt < MAX_RETRIES) {
        Serial.printf("[ESP-NOW RETRY] Attempt %d timed out. Retrying in 100ms...\n", attempt);
        delay(100);
      } else {
        Serial.println("[ESP-NOW WARNING] Max retries reached without ACK confirmation.");
      }
    }
  }

  Serial.println();
  Serial.printf("Clean shutdown & entering ESP32 Deep Sleep for %u seconds...\n", sleepDurationSeconds);
  Serial.println("--------------------------------------------------");
  Serial.flush();

  // Enter ESP32 Deep Sleep using built-in hardware RTC timer
  esp_sleep_enable_timer_wakeup((uint64_t)sleepDurationSeconds * 1000000ULL);
  esp_deep_sleep_start();
}

//==================================================
// MAIN LOOP (Unused when using Deep Sleep)
//==================================================
void loop() {
  // Execution never reaches here when using esp_deep_sleep_start() in setup()
}
