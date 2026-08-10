/*
 * Arduino Relay Controller for Water Pump Motor
 * 
 * Selective Printing Policy:
 * - Prints ONLY when Config changes (min, max, auto)
 * - Prints ONLY when Water Level Percentage changes
 * - Prints ONLY when Motor Status changes (Relay Pin 7 toggles)
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
const bool RELAY_ACTIVE_LOW = false; // Set to true if your relay triggers ON with LOW

//==================================================
// ARDUINO LOCAL UNDERSTOOD STATE
//==================================================
int arduinoMin = -1;
int arduinoMax = -1;
bool arduinoAutoMode = false;
bool isConfigInitialized = false;

int arduinoWaterLevel = -1;
String arduinoMotorStatus = "unknown";

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
          Serial.println("[MOTOR STATUS CHANGED] Motor: RUNNING (Relay Pin 7 HIGH)");
        }
        sendArduinoStateToESP8266();
      } else if (incoming.equalsIgnoreCase("MOTOR_OFF") || incoming.equalsIgnoreCase("STOP")) {
        if (arduinoMotorStatus != "stopped") {
          arduinoMotorStatus = "stopped";
          applyRelayHardwareState(false);
          Serial.println("[MOTOR STATUS CHANGED] Motor: STOPPED (Relay Pin 7 LOW)");
        }
        sendArduinoStateToESP8266();
      }
      return;
    }

    String msgType = doc["type"] | "";

    //==================================================
    // TYPE 1: SETTINGS JSON ({"type":"settings","min":33,"max":100,"auto":true})
    //==================================================
    if (msgType == "settings") {
      int newMin = doc["min"] | 33;
      int newMax = doc["max"] | 100;
      bool newAuto = doc["auto"] | false;

      // Print ONLY if config changed or not yet initialized
      if (!isConfigInitialized || newMin != arduinoMin || newMax != arduinoMax || newAuto != arduinoAutoMode) {
        arduinoMin = newMin;
        arduinoMax = newMax;
        arduinoAutoMode = newAuto;
        isConfigInitialized = true;

        Serial.print("[CONFIG CHANGED] Min: ");
        Serial.print(arduinoMin);
        Serial.print("%, Max: ");
        Serial.print(arduinoMax);
        Serial.print("%, Auto Mode: ");
        Serial.println(arduinoAutoMode ? "ON" : "OFF");
      }

      sendArduinoStateToESP8266();
    }
    //==================================================
    // TYPE 2: STATUS / PERCENTAGE JSON ({"type":"status","level":66,"motor":"stopped"})
    //==================================================
    else if (msgType == "status") {
      int newLevel = doc["level"] | 0;
      String newMotor = doc["motor"] | "stopped";

      // Check if percentage (water level) changed
      if (newLevel != arduinoWaterLevel) {
        arduinoWaterLevel = newLevel;
        Serial.print("[PERCENTAGE CHANGED] Water Level: ");
        Serial.print(arduinoWaterLevel);
        Serial.println("%");
      }

      // Check if motor status changed
      if (newMotor != arduinoMotorStatus) {
        arduinoMotorStatus = newMotor;
        bool turnOn = (arduinoMotorStatus == "started" || arduinoMotorStatus == "front");
        applyRelayHardwareState(turnOn);

        Serial.print("[MOTOR STATUS CHANGED] Motor: ");
        if (turnOn) {
          Serial.println("RUNNING (Relay Pin 7 HIGH)");
        } else {
          Serial.println("STOPPED (Relay Pin 7 LOW)");
        }
      }

      sendArduinoStateToESP8266();
    }
  }

  delay(10);
}
