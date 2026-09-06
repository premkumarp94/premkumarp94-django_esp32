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

const char* DEVICE_ID = "esp8266_device_01";

String pendingDeviceMsg = "esp8266 tank monitor booted normally";

int iterationCount = 0;


//==================================================
// WATER PROBES (4 Level Probes + Common Reference)
//==================================================
// Probe 1: 25% (Low level)
// Probe 2: 50% (Mid-Low level)
// Probe 3: 75% (Mid-High level)
// Probe 4: 100% (Full level)

const int COMMON_PIN    = D1; // GND reference for water (GPIO5)
const int PROBE_25_PIN  = D2; // 25% Probe  (GPIO4)
const int PROBE_50_PIN  = D5; // 50% Probe  (GPIO14)
const int PROBE_75_PIN  = D6; // 75% Probe  (GPIO12)
const int PROBE_100_PIN = D7; // 100% Probe (GPIO13)

// Set to true to use ESP8266 internal INPUT_PULLUP resistors (No external resistors needed!).
// Disconnected/air = HIGH on pin, Water touching COMMON_PIN(GND) = LOW on pin.
const bool USE_INTERNAL_PULLUP = true;


//==================================================
// TELEMETRY SERVERS
//==================================================

const char* serverList[] = {
  "http://192.168.1.1:8000/api/telemetry/",
  "http://192.168.1.2:8000/api/telemetry/",
  "http://192.168.1.3:8000/api/telemetry/",
  "http://192.168.1.4:8000/api/telemetry/",
  "http://192.168.1.5:8000/api/telemetry/",
  "https://premkumarp94.pythonanywhere.com/api/telemetry/"
};

const int SERVER_COUNT =
    sizeof(serverList) / sizeof(serverList[0]);

int activeServer = -1;


//==================================================
// WIFI CONNECTION
//==================================================

bool connectWiFi()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    return true;
  }

  Serial.println();
  Serial.print("Connecting WiFi");

  WiFi.begin(ssid, password);

  int attempts = 0;

  while (WiFi.status() != WL_CONNECTED && attempts < 30)
  {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.print("Connected. IP: ");
    Serial.println(WiFi.localIP());

    return true;
  }

  Serial.println("WiFi connection failed.");

  return false;
}


//==================================================
// WATER LEVEL MEASUREMENT (4 Probes)
//==================================================

int readWaterLevel()
{
  if (USE_INTERNAL_PULLUP)
  {
    // COMMON_PIN output is LOW (GND)
    digitalWrite(COMMON_PIN, LOW);
    delay(50);

    // Active-LOW: In water = LOW (0), Disconnected/Air = HIGH (1 due to pull-up)
    bool p25  = (digitalRead(PROBE_25_PIN)  == LOW);
    bool p50  = (digitalRead(PROBE_50_PIN)  == LOW);
    bool p75  = (digitalRead(PROBE_75_PIN)  == LOW);
    bool p100 = (digitalRead(PROBE_100_PIN) == LOW);

    Serial.println();
    Serial.println("===== 4-PROBE TANK LEVEL TEST (INPUT_PULLUP Active-LOW) =====");
    Serial.print("Probe 1 (25%  - D2/GPIO4)  = "); Serial.println(p25  ? "DETECTED (WATER)" : "OPEN (AIR)");
    Serial.print("Probe 2 (50%  - D5/GPIO14) = "); Serial.println(p50  ? "DETECTED (WATER)" : "OPEN (AIR)");
    Serial.print("Probe 3 (75%  - D6/GPIO12) = "); Serial.println(p75  ? "DETECTED (WATER)" : "OPEN (AIR)");
    Serial.print("Probe 4 (100% - D7/GPIO13) = "); Serial.println(p100 ? "DETECTED (WATER)" : "OPEN (AIR)");

    if (p100) return 100;
    if (p75)  return 75;
    if (p50)  return 50;
    if (p25)  return 25;
    return 0;
  }
  else
  {
    // Active-HIGH Mode (Requires external pull-down resistors to GND on probes)
    digitalWrite(COMMON_PIN, HIGH);
    delay(50);

    bool p25  = (digitalRead(PROBE_25_PIN)  == HIGH);
    bool p50  = (digitalRead(PROBE_50_PIN)  == HIGH);
    bool p75  = (digitalRead(PROBE_75_PIN)  == HIGH);
    bool p100 = (digitalRead(PROBE_100_PIN) == HIGH);

    digitalWrite(COMMON_PIN, LOW);

    Serial.println();
    Serial.println("===== 4-PROBE TANK LEVEL TEST (External Pull-down Active-HIGH) =====");
    Serial.print("Probe 1 (25%  - D2/GPIO4)  = "); Serial.println(p25  ? "DETECTED (WATER)" : "OPEN (AIR)");
    Serial.print("Probe 2 (50%  - D5/GPIO14) = "); Serial.println(p50  ? "DETECTED (WATER)" : "OPEN (AIR)");
    Serial.print("Probe 3 (75%  - D6/GPIO12) = "); Serial.println(p75  ? "DETECTED (WATER)" : "OPEN (AIR)");
    Serial.print("Probe 4 (100% - D7/GPIO13) = "); Serial.println(p100 ? "DETECTED (WATER)" : "OPEN (AIR)");

    if (p100) return 100;
    if (p75)  return 75;
    if (p50)  return 50;
    if (p25)  return 25;
    return 0;
  }
}

//==================================================
// HTTP POST JSON
//==================================================

bool postJson(
  const char* url,
  const String& body,
  String& responseOut
)
{
  responseOut = "";

  HTTPClient http;

  int httpCode = -1;

  Serial.print("POST: ");
  Serial.println(url);

  if (strncmp(url, "https://", 8) == 0)
  {
    WiFiClientSecure client;
    client.setInsecure();

    if (!http.begin(client, url))
    {
      Serial.println("HTTPS begin failed.");
      return false;
    }

    http.addHeader("Content-Type", "application/json");
    http.setTimeout(5000);

    httpCode = http.POST(body);

    if (httpCode > 0)
    {
      responseOut = http.getString();
    }

    http.end();
  }
  else
  {
    WiFiClient client;

    if (!http.begin(client, url))
    {
      Serial.println("HTTP begin failed.");
      return false;
    }

    http.addHeader("Content-Type", "application/json");
    http.setTimeout(5000);

    httpCode = http.POST(body);

    if (httpCode > 0)
    {
      responseOut = http.getString();
    }

    http.end();
  }

  Serial.print("HTTP code: ");
  Serial.println(httpCode);

  if (httpCode >= 200 && httpCode < 300)
  {
    return true;
  }

  if (httpCode > 0)
  {
    Serial.println("Server response:");
    Serial.println(responseOut);
  }

  return false;
}


//==================================================
// SERVER DISCOVERY
//==================================================

bool discoverServer()
{
  Serial.println();
  Serial.println("================================");
  Serial.println("Searching for telemetry server");
  Serial.println("================================");

  for (int i = 0; i < SERVER_COUNT; i++)
  {
    StaticJsonDocument<256> doc;

    doc["id"] = DEVICE_ID;
    doc["message"] = "ping";

    String body;
    serializeJson(doc, body);

    String response;

    Serial.print("Trying ");
    Serial.println(serverList[i]);

    if (postJson(serverList[i], body, response))
    {
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

String buildTelemetry(
  const String& ack,
  const String& message
)
{
  StaticJsonDocument<512> doc;

  doc["id"] = DEVICE_ID;

  JsonObject sensor =
      doc.createNestedObject("sensor values");

  sensor["water_level"] = readWaterLevel();

  doc["ack"] = ack;
  doc["message"] = message;

  String body;
  serializeJson(doc, body);

  return body;
}


//==================================================
// SEND TELEMETRY
//==================================================

bool sendTelemetry()
{
  if (activeServer == -1)
  {
    if (!discoverServer())
    {
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

  bool success =
      postJson(
        serverList[activeServer],
        body,
        response
      );

  if (!success)
  {
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

void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("==========================================");
  Serial.println("ESP8266 4-PROBE TANK LEVEL MONITOR");
  Serial.println("==========================================");
  Serial.print("Device ID: ");
  Serial.println(DEVICE_ID);


  //================================================
  // WATER PROBE PIN INITIALIZATION
  //================================================

  pinMode(COMMON_PIN, OUTPUT);

  if (USE_INTERNAL_PULLUP)
  {
    digitalWrite(COMMON_PIN, LOW);
    pinMode(PROBE_25_PIN,  INPUT_PULLUP);
    pinMode(PROBE_50_PIN,  INPUT_PULLUP);
    pinMode(PROBE_75_PIN,  INPUT_PULLUP);
    pinMode(PROBE_100_PIN, INPUT_PULLUP);
  }
  else
  {
    digitalWrite(COMMON_PIN, LOW);
    pinMode(PROBE_25_PIN,  INPUT);
    pinMode(PROBE_50_PIN,  INPUT);
    pinMode(PROBE_75_PIN,  INPUT);
    pinMode(PROBE_100_PIN, INPUT);
  }


  //================================================
  // WIFI & SERVER DISCOVERY
  //================================================

  if (connectWiFi())
  {
    discoverServer();
  }
}


//==================================================
// LOOP
//==================================================

void loop()
{
  // Make sure WiFi is connected
  if (!connectWiFi())
  {
    delay(2000);
    return;
  }

  // Send normal telemetry
  sendTelemetry();

  if (iterationCount == 0)
  {
    pendingDeviceMsg = "came to first iteration";
  }

  iterationCount++;

  Serial.println();
  Serial.print("Iteration: ");
  Serial.println(iterationCount);
  Serial.println("------------------------------------");

  delay(5000);
}