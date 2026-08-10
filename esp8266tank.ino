#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

//==================================================
// WIFI
//==================================================

const char* ssid = "TIC_5G-PREM";
const char* password = "prem@123";

//==================================================
// DEVICE
//==================================================

const char* DEVICE_ID = "esp8266_device_01";

String motorStatus = "stopped";

String pendingDeviceMsg = "esp8266 booted normally";

int iterationCount = 0;


//==================================================
// WATER PROBES
//==================================================

const int COMMON_PIN = D1;
const int LOW_PIN    = D2;
const int MID_PIN    = D5;
const int FULL_PIN   = D6;


//==================================================
// SERVERS
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
// WATER LEVEL
//==================================================

int readWaterLevel()
{
  digitalWrite(COMMON_PIN, HIGH);
  delay(50);

  int low  = digitalRead(LOW_PIN);
  int mid  = digitalRead(MID_PIN);
  int full = digitalRead(FULL_PIN);

  digitalWrite(COMMON_PIN, LOW);

  Serial.println();
  Serial.println("===== PROBE TEST =====");
  Serial.print("LOW  (D2/GPIO4)  = ");
  Serial.println(low);

  Serial.print("MID  (D5/GPIO14) = ");
  Serial.println(mid);

  Serial.print("FULL (D6/GPIO12) = ");
  Serial.println(full);

  if (full == HIGH)
    return 100;

  if (mid == HIGH)
    return 66;

  if (low == HIGH)
    return 33;

  return 0;
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

    // Testing only.
    // This skips certificate verification.
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

  sensor["motor_status"] = motorStatus;

  doc["ack"] = ack;

  doc["message"] = message;

  String body;

  serializeJson(doc, body);

  return body;
}


//==================================================
// SEND NORMAL TELEMETRY
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

  String body =
      buildTelemetry(ack, message);

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


  //================================================
  // PARSE SERVER RESPONSE
  //================================================

  StaticJsonDocument<512> responseDoc;

  DeserializationError error =
      deserializeJson(
        responseDoc,
        response
      );

  if (error)
  {
    Serial.print("JSON parse error: ");

    Serial.println(error.c_str());

    return true;
  }

  String command =
      responseDoc["command"] | "";

  Serial.print("Command received: ");
  Serial.println(command);


  //================================================
  // EXECUTE COMMAND
  //================================================

  bool commandExecuted = false;

  if (command == "start_motor")
  {
    Serial.println();
    Serial.println(">>> START MOTOR COMMAND <<<");

    //================================================
    // ACTUAL RELAY CONTROL GOES HERE
    // digitalWrite(RELAY_PIN, HIGH);
    //================================================

    motorStatus = "started";

    pendingDeviceMsg =
        "Motor started successfully.";

    commandExecuted = true;
  }

  else if (command == "stop_motor")
  {
    Serial.println();
    Serial.println(">>> STOP MOTOR COMMAND <<<");

    //================================================
    // ACTUAL RELAY CONTROL GOES HERE
    // digitalWrite(RELAY_PIN, LOW);
    //================================================

    motorStatus = "stopped";

    pendingDeviceMsg =
        "Motor stopped successfully.";

    commandExecuted = true;
  }

  else
  {
    if (command != "" &&
        command != "none")
    {
      Serial.print("Invalid command: ");
      Serial.println(command);
    }
    else
    {
      Serial.println("No command received.");
    }
  }


  //================================================
  // IMMEDIATE COMMAND CONFIRMATION
  //================================================

  if (commandExecuted)
  {
    Serial.println();
    Serial.println(
      "Sending immediate command confirmation..."
    );

    String confirmBody =
        buildTelemetry(
          "cmd_executed_ack",
          pendingDeviceMsg
        );

    Serial.println("Confirmation JSON:");

    Serial.println(confirmBody);

    String confirmResponse;

    bool confirmSuccess =
        postJson(
          serverList[activeServer],
          confirmBody,
          confirmResponse
        );

    if (confirmSuccess)
    {
      Serial.println(
        "Server state updated immediately!"
      );

      Serial.println(
        "Confirmation response:"
      );

      Serial.println(confirmResponse);

      pendingDeviceMsg = "";
    }
    else
    {
      Serial.println(
        "Immediate confirmation failed."
      );

      // IMPORTANT:
      // Keep the message so the next telemetry
      // attempt can send it.
    }
  }

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
  Serial.println("====================================");
  Serial.println("ESP8266 TELEMETRY DEVICE STARTING");
  Serial.println("====================================");

  Serial.print("Device ID: ");
  Serial.println(DEVICE_ID);


  //================================================
  // WATER PROBES
  //================================================

  pinMode(COMMON_PIN, OUTPUT);

  digitalWrite(COMMON_PIN, LOW);

  pinMode(LOW_PIN, INPUT);
  pinMode(MID_PIN, INPUT);
  pinMode(FULL_PIN, INPUT);


  //================================================
  // WIFI
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
  // Make sure WiFi is available
  if (!connectWiFi())
  {
    delay(2000);

    return;
  }


  //================================================
  // NORMAL TELEMETRY
  //================================================

  sendTelemetry();


  //================================================
  // SAME BEHAVIOR AS PYTHON SIMULATOR
  //================================================

  if (iterationCount == 0)
  {
    pendingDeviceMsg =
        "came to first iteration";
  }

  iterationCount++;

  Serial.println();
  Serial.print("Iteration: ");
  Serial.println(iterationCount);

  Serial.println("------------------------------------");

  delay(5000);
}