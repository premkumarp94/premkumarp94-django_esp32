/*
 * Arduino Relay Controller for Water Pump Motor
 * 
 * Hardware Setup:
 * - Arduino Digital Pin 7 -> Relay Module Signal Pin (IN / S)
 * - Arduino GND            -> Relay Module GND & ESP8266 GND (Shared GND)
 * - Arduino RX (Pin 0)     -> ESP8266 TX (GPIO1 / D10 or SoftwareSerial)
 * 
 * Serial Commands Received from ESP8266:
 * - "MOTOR_ON"  or "START" -> Turns Relay ON (Pin 7)
 * - "MOTOR_OFF" or "STOP"  -> Turns Relay OFF (Pin 7)
 */

// Pin definition for Relay
const int RELAY_PIN = 7;

// Active LOW Relay configuration:
// Set to true if your Relay module triggers ON with LOW (0V).
// Set to false if your Relay module triggers ON with HIGH (5V).
const bool RELAY_ACTIVE_LOW = false;

// Track current motor state
bool isMotorOn = false;

void setRelayState(bool turnOn) {
  isMotorOn = turnOn;
  if (RELAY_ACTIVE_LOW) {
    digitalWrite(RELAY_PIN, turnOn ? LOW : HIGH);
  } else {
    digitalWrite(RELAY_PIN, turnOn ? HIGH : LOW);
  }
  
  Serial.println();
  Serial.print(">>> ARDUINO RELAY PIN 7 STATE UPDATED: ");
  Serial.println(turnOn ? "RELAY ON (MOTOR RUNNING) <<<" : "RELAY OFF (MOTOR STOPPED) <<<");
}

void setup() {
  // Initialize Serial communication (matching ESP8266 baud rate)
  Serial.begin(115200);
  delay(500);

  // Configure Pin 7 as OUTPUT
  pinMode(RELAY_PIN, OUTPUT);

  // Default state: Motor OFF
  setRelayState(false);

  Serial.println();
  Serial.println("==================================================");
  Serial.println("ARDUINO RELAY CONTROLLER INITIALIZED");
  Serial.println("Relay Pin      : 7");
  Serial.print  ("Relay Logic    : ");
  Serial.println(RELAY_ACTIVE_LOW ? "Active LOW" : "Active HIGH");
  Serial.println("Listening for ESP8266 Serial Commands (MOTOR_ON / MOTOR_OFF)...");
  Serial.println("==================================================");
}

void loop() {
  // Check if Serial command received from ESP8266
  if (Serial.available() > 0) {
    String incoming = Serial.readStringUntil('\n');
    incoming.trim();

    if (incoming.length() == 0) return;

    Serial.print("Received Command from ESP8266: ");
    Serial.println(incoming);

    if (incoming.equalsIgnoreCase("MOTOR_ON") || incoming.equalsIgnoreCase("START") || incoming.equalsIgnoreCase("start_motor")) {
      setRelayState(true);
    } else if (incoming.equalsIgnoreCase("MOTOR_OFF") || incoming.equalsIgnoreCase("STOP") || incoming.equalsIgnoreCase("stop_motor")) {
      setRelayState(false);
    } else if (incoming.startsWith("set_threshold") || incoming.startsWith("set_settings")) {
      Serial.print("Settings Command Received: ");
      Serial.println(incoming);
    } else {
      Serial.print("Unrecognized Serial Data: ");
      Serial.println(incoming);
    }
  }

  delay(10);
}
