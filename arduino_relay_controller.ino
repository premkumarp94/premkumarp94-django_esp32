/*
 * Arduino Relay Controller for Water Pump Motor
 * 
 * Selective Printing Policy:
 * - Parses IST Timestamp ("time":"YYYYMMDD:HH:MM:SS") from ESP8266 JSON packets.
 * - DOES NOT print if ONLY the datetime changed while state data remains unchanged.
 * - Prints ONLY when Config changes (min, max, auto).
 * - Prints ONLY when Water Level Percentage changes.
 * - Prints ONLY when Motor Status changes (Relay Pin 7 toggles).
 * 
 * Hardware Connections:
 * - Relay Signal (IN/S) -> Arduino Digital Pin 7
 * - Shared GND          -> ESP8266 GND & Relay GND
 * - Arduino RX (Pin 0)  -> ESP8266 TX (GPIO1 / D10)
 */

#include <ArduinoJson.h>

//==================================================
// PIN DEFINITIONS & RELAY LOGIC
//==================================================
const int RELAY_PIN = 7;
const bool RELAY_ACTIVE_LOW = true; // Set to true for Active-LOW Relay modules (0V/LOW = ON)

//==================================================
// ARDUINO LOCAL UNDERSTOOD STATE
//==================================================
int arduinoMin = -1;
int arduinoMax = -1;
bool arduinoAutoMode = false;
bool isConfigInitialized = false;

int arduinoWaterLevel = -1;
String arduinoMotorStatus = "unknown";
String lastIstTime = "";

// Set relay hardware pin state
void applyRelayHardwareState(bool turnOn) {
  if (RELAY_ACTIVE_LOW) {
    digitalWrite(RELAY_PIN, turnOn ? LOW : HIGH);
  } else {
    digitalWrite(RELAY_PIN, turnOn ? HIGH : LOW);
  }
}

// Respond over Serial with Arduino's current state JSON to ESP8266
void sendArduinoStateToESP8266() {
  StaticJsonDocument<256> doc;
  doc["type"] = "state";
  doc["min"] = (arduinoMin == -1) ? 33 : arduinoMin;
  doc["max"] = (arduinoMax == -1) ? 100 : arduinoMax;
  doc["auto"] = arduinoAutoMode;
  doc["level"] = (arduinoWaterLevel == -1) ? 0 : arduinoWaterLevel;
  doc["motor"] = (arduinoMotorStatus == "unknown") ? "stopped" : arduinoMotorStatus;
  doc["time"] = lastIstTime;

  String output;
  serializeJson(doc, output);
  Serial.println(output);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(RELAY_PIN, OUTPUT);
  applyRelayHardwareState(false);

  // Silent startup - Arduino does not spam startup banners
}

void loop() {
  if (Serial.available() > 0) {
    String incoming = Serial.readStringUntil('\n');
    incoming.trim();

    if (incoming.length() == 0) return;

    // Parse incoming JSON message from ESP8266
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, incoming);

    if (error) {
      // Fallback for legacy plain text commands
      if (incoming.equalsIgnoreCase("MOTOR_ON") || incoming.equalsIgnoreCase("START")) {
        if (arduinoMotorStatus != "started") {
          arduinoMotorStatus = "started";
          applyRelayHardwareState(true);
          Serial.print("[MOTOR STATUS CHANGED] Motor: RUNNING (Relay Pin 7 ");
          Serial.print(RELAY_ACTIVE_LOW ? "LOW" : "HIGH");
          Serial.println(")");
        }
        sendArduinoStateToESP8266();
      } else if (incoming.equalsIgnoreCase("MOTOR_OFF") || incoming.equalsIgnoreCase("STOP")) {
        if (arduinoMotorStatus != "stopped") {
          arduinoMotorStatus = "stopped";
          applyRelayHardwareState(false);
          Serial.print("[MOTOR STATUS CHANGED] Motor: STOPPED (Relay Pin 7 ");
          Serial.print(RELAY_ACTIVE_LOW ? "HIGH" : "LOW");
          Serial.println(")");
        }
        sendArduinoStateToESP8266();
      }
      return;
    }

    String msgType = doc["type"] | "";
    if (doc.containsKey("time")) {
      lastIstTime = doc["time"].as<String>();
    }

    //==================================================
    // TYPE 1: SETTINGS JSON ({"type":"settings","min":33,"max":100,"auto":true,"time":"YYYYMMDD:HH:MM:SS"})
    //==================================================
    if (msgType == "settings") {
      int newMin = doc["min"] | 33;
      int newMax = doc["max"] | 100;
      bool newAuto = doc["auto"] | false;

      // Ignore pure datetime updates! Print ONLY if config values actually changed
      if (!isConfigInitialized || newMin != arduinoMin || newMax != arduinoMax || newAuto != arduinoAutoMode) {
        arduinoMin = newMin;
        arduinoMax = newMax;
        arduinoAutoMode = newAuto;
        isConfigInitialized = true;

        Serial.print("[");
        Serial.print(lastIstTime);
        Serial.print("] [CONFIG CHANGED] Min: ");
        Serial.print(arduinoMin);
        Serial.print("%, Max: ");
        Serial.print(arduinoMax);
        Serial.print("%, Auto Mode: ");
        Serial.println(arduinoAutoMode ? "ON" : "OFF");
      }

      sendArduinoStateToESP8266();
    }
    //==================================================
    // TYPE 2: STATUS / PERCENTAGE JSON ({"type":"status","level":66,"motor":"stopped","time":"YYYYMMDD:HH:MM:SS"})
    //==================================================
    else if (msgType == "status") {
      int newLevel = doc["level"] | 0;
      String newMotor = doc["motor"] | "stopped";

      // Ignore pure datetime updates! Print ONLY if water level percentage changed
      if (newLevel != arduinoWaterLevel) {
        arduinoWaterLevel = newLevel;
        Serial.print("[");
        Serial.print(lastIstTime);
        Serial.print("] [PERCENTAGE CHANGED] Water Level: ");
        Serial.print(arduinoWaterLevel);
        Serial.println("%");
      }

      // Ignore pure datetime updates! Print ONLY if motor status changed
      if (newMotor != arduinoMotorStatus) {
        arduinoMotorStatus = newMotor;
        bool turnOn = (arduinoMotorStatus == "started" || arduinoMotorStatus == "front");
        applyRelayHardwareState(turnOn);

        Serial.print("[");
        Serial.print(lastIstTime);
        Serial.print("] [MOTOR STATUS CHANGED] Motor: ");
        if (turnOn) {
          Serial.print("RUNNING (Relay Pin 7 ");
          Serial.print(RELAY_ACTIVE_LOW ? "LOW" : "HIGH");
          Serial.println(")");
        } else {
          Serial.print("STOPPED (Relay Pin 7 ");
          Serial.print(RELAY_ACTIVE_LOW ? "HIGH" : "LOW");
          Serial.println(")");
        }
      }

      sendArduinoStateToESP8266();
    }
  }

  delay(10);
}
