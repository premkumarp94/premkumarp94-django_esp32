#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

//===================== WIFI =====================
const char* ssid = "TIC_5G-PREM";
const char* password = "prem@123";

//===================== DEVICE ===================
String deviceId = "esp32_device_01";
String motorStatus = "stopped";
String pendingDeviceMsg = "esp32 booted normally";
int iterationCount = 0;

//===================== PROBES ===================
const int COMMON_PIN = 23;
const int LOW_PIN    = 21;
const int MID_PIN    = 19;
const int FULL_PIN   = 18;

//===================== SERVER LIST ==============
String serverList[] = {
  "http://192.168.1.1:8000/api/telemetry/",
  "http://192.168.1.2:8000/api/telemetry/",
  "http://192.168.1.3:8000/api/telemetry/",
  "http://192.168.1.4:8000/api/telemetry/",
  "http://192.168.1.5:8000/api/telemetry/",
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

//------------------------------------------------
int readWaterLevel() {
  digitalWrite(COMMON_PIN, HIGH);
  delay(20);

  bool low = digitalRead(LOW_PIN);
  bool mid = digitalRead(MID_PIN);
  bool full = digitalRead(FULL_PIN);

  digitalWrite(COMMON_PIN, LOW);

  Serial.printf("LOW=%d MID=%d FULL=%d\n", low, mid, full);

  if (full) return 100;
  if (mid)  return 66;
  if (low)  return 33;
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
  sensor["motor_status"] = motorStatus;

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

  DynamicJsonDocument resp(512);

  if (deserializeJson(resp, response) == DeserializationError::Ok) {
    String cmd = resp["command"] | "";

    if (cmd == "start_motor") {
      motorStatus = "started";
      pendingDeviceMsg = "Motor started successfully.";
      Serial.println("START command");
      sendTelemetry("cmd_executed_ack", pendingDeviceMsg);
      pendingDeviceMsg = "";
    } else if (cmd == "stop_motor") {
      motorStatus = "stopped";
      pendingDeviceMsg = "Motor stopped successfully.";
      Serial.println("STOP command");
      sendTelemetry("cmd_executed_ack", pendingDeviceMsg);
      pendingDeviceMsg = "";
    } else {
      Serial.println("No command");
    }
  }

  return true;
}

//------------------------------------------------
void setup() {
  Serial.begin(115200);

  pinMode(COMMON_PIN, OUTPUT);
  digitalWrite(COMMON_PIN, LOW);

  pinMode(LOW_PIN, INPUT_PULLDOWN);
  pinMode(MID_PIN, INPUT_PULLDOWN);
  pinMode(FULL_PIN, INPUT_PULLDOWN);

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

  delay(2000);
}


