#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

//===================== WIFI =====================
const char* ssid = "TIC_5G-PREM";
const char* password = "prem@123";

//===================== DEVICE ===================
String deviceId = "esp8266_device_01";
String pendingDeviceMsg = "esp32 tank monitor booted normally";
int iterationCount = 0;

//===================== 4 WATER PROBES ===========
const int COMMON_PIN    = 23; // Reference pin
const int PROBE_25_PIN  = 21; // 25% probe
const int PROBE_50_PIN  = 19; // 50% probe
const int PROBE_75_PIN  = 18; // 75% probe
const int PROBE_100_PIN = 5;  // 100% probe

//===================== SERVER LIST ==============
String serverList[] = {
  "https://premkumarp94.pythonanywhere.com/api/telemetry/"
};

const int SERVER_COUNT = sizeof(serverList) / sizeof(serverList[0]);
int activeServer = -1;

//------------------------------------------------
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.print("Connecting WiFi");
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("Connected. IP : ");
  Serial.println(WiFi.localIP());
}

//===================== PROBE DETECTION STATES ===
bool probe25Detected  = false;
bool probe50Detected  = false;
bool probe75Detected  = false;
bool probe100Detected = false;

//------------------------------------------------
int readWaterLevel() {
  // Step 1: Discharge any residual capacitance/static on probe pins to GND
  pinMode(PROBE_25_PIN,  OUTPUT); digitalWrite(PROBE_25_PIN,  LOW);
  pinMode(PROBE_50_PIN,  OUTPUT); digitalWrite(PROBE_50_PIN,  LOW);
  pinMode(PROBE_75_PIN,  OUTPUT); digitalWrite(PROBE_75_PIN,  LOW);
  pinMode(PROBE_100_PIN, OUTPUT); digitalWrite(PROBE_100_PIN, LOW);
  delay(5);

  // Step 2: Switch probe pins to High-Impedance INPUT mode (no pulldown load)
  // This allows even weak currents through high-resistance water to pull the pin HIGH
  pinMode(PROBE_25_PIN,  INPUT);
  pinMode(PROBE_50_PIN,  INPUT);
  pinMode(PROBE_75_PIN,  INPUT);
  pinMode(PROBE_100_PIN, INPUT);

  // Step 3: Pulse reference COMMON pin HIGH (3.3V)
  digitalWrite(COMMON_PIN, HIGH);
  delay(30); // Allow voltage to stabilize across the water column

  // Step 4: Sample probe states
  probe25Detected  = digitalRead(PROBE_25_PIN);
  probe50Detected  = digitalRead(PROBE_50_PIN);
  probe75Detected  = digitalRead(PROBE_75_PIN);
  probe100Detected = digitalRead(PROBE_100_PIN);

  // Step 5: Turn OFF reference pin and re-engage pulldowns to prevent electrolysis
  digitalWrite(COMMON_PIN, LOW);
  pinMode(PROBE_25_PIN,  INPUT_PULLDOWN);
  pinMode(PROBE_50_PIN,  INPUT_PULLDOWN);
  pinMode(PROBE_75_PIN,  INPUT_PULLDOWN);
  pinMode(PROBE_100_PIN, INPUT_PULLDOWN);

  Serial.printf("P25=%d P50=%d P75=%d P100=%d\n", 
                probe25Detected, probe50Detected, probe75Detected, probe100Detected);

  if (probe100Detected) return 100;
  if (probe75Detected)  return 75;
  if (probe50Detected)  return 50;
  if (probe25Detected)  return 25;
  return 0;
}

//------------------------------------------------
bool postJson(String url, String body, String &responseOut) {
  HTTPClient http;
  int code = -1;

  if (url.startsWith("https://")) {
    WiFiClientSecure client;
    client.setInsecure();
    if (http.begin(client, url)) {
      http.addHeader("Content-Type", "application/json");
      http.setTimeout(5000);
      code = http.POST(body);
      if (code > 0) {
        responseOut = http.getString();
      }
      http.end();
    }
  } else {
    if (http.begin(url)) {
      http.addHeader("Content-Type", "application/json");
      http.setTimeout(3000);
      code = http.POST(body);
      if (code > 0) {
        responseOut = http.getString();
      }
      http.end();
    }
  }

  return (code == 200);
}

//------------------------------------------------
bool discoverServer() {
  Serial.println("\nSearching for server...");

  for (int i = 0; i < SERVER_COUNT; i++) {
    StaticJsonDocument<128> doc;
    doc["id"] = deviceId;
    doc["message"] = "ping";

    String body;
    serializeJson(doc, body);

    String resp;

    Serial.print("Trying ");
    Serial.println(serverList[i]);

    if (postJson(serverList[i], body, resp)) {
      activeServer = i;
      Serial.print("Using ");
      Serial.println(serverList[i]);
      return true;
    }
  }

  activeServer = -1;
  return false;
}

//------------------------------------------------
bool sendTelemetry(String ack, String msg) {
  if (activeServer == -1) {
    if (!discoverServer()) return false;
  }

  DynamicJsonDocument doc(512);

  doc["id"] = deviceId;

  JsonObject sensor = doc.createNestedObject("sensor values");
  sensor["water_level"] = readWaterLevel();
  sensor["probe_25"]    = probe25Detected;
  sensor["probe_50"]    = probe50Detected;
  sensor["probe_75"]    = probe75Detected;
  sensor["probe_100"]   = probe100Detected;

  doc["ack"] = ack;
  doc["message"] = msg;

  String body;
  serializeJson(doc, body);

  Serial.println("\nSending:");
  Serial.println(body);

  String response;

  if (!postJson(serverList[activeServer], body, response)) {
    Serial.println("Server failed. Rediscovering...");
    activeServer = -1;
    return false;
  }

  Serial.println("Response:");
  Serial.println(response);

  return true;
}

//------------------------------------------------
void setup() {
  Serial.begin(115200);

  pinMode(COMMON_PIN, OUTPUT);
  digitalWrite(COMMON_PIN, LOW);

  pinMode(PROBE_25_PIN,  INPUT_PULLDOWN);
  pinMode(PROBE_50_PIN,  INPUT_PULLDOWN);
  pinMode(PROBE_75_PIN,  INPUT_PULLDOWN);
  pinMode(PROBE_100_PIN, INPUT_PULLDOWN);

  connectWiFi();
  discoverServer();
}

//------------------------------------------------
void loop() {
  connectWiFi();

  sendTelemetry("dummy_ack", pendingDeviceMsg);

  pendingDeviceMsg = "";

  if (iterationCount == 0)
    pendingDeviceMsg = "came to first iteration";

  iterationCount++;

  delay(5000);
}
