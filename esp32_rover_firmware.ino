/*
 * AGRIVISION ROVER CONTROLLER FIRMWARE
 * Target: ESP32-S3 Microcontroller
 * Project: Real-Time Foliar Crop Disease Detection & Telemetry
 * Site: Alliance University Lot 3 Field Trials, Anekal Rural, Karnataka
 *
 * Hardware Pinouts & Circuit Configuration:
 *  - L298N Dual H-Bridge:
 *      ENA -> GPIO 14 (PWM Motor A)
 *      IN1 -> GPIO 27 (Left Motor Forward)
 *      IN2 -> GPIO 26 (Left Motor Reverse)
 *      IN3 -> GPIO 25 (Right Motor Forward)
 *      IN4 -> GPIO 33 (Right Motor Reverse)
 *      ENB -> GPIO 32 (PWM Motor B)
 *  - HC-SR04 Ultrasonic Sonar:
 *      TRIG -> GPIO 12
 *      ECHO -> GPIO 13 (with voltage divider to protect 3.3V GPIO)
 *      Auto-Stop Threshold: < 20 cm
 *  - GalaxyCore GC2145 DVP Camera Sensor:
 *      PCLK -> 10 MHz DVP Pixel Clock
 *      XCLK -> GPIO 0, VSYNC -> GPIO 5, HREF -> GPIO 23
 *  - NEO-6M / M8N GPS Module:
 *      TX -> GPIO 16 (Serial2 RX)
 *      RX -> GPIO 17 (Serial2 TX)
 *  - 7.42V 2S LiPo Battery ADC:
 *      Divider -> GPIO 34 (100k + 100k ohm)
 */

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

const char* ap_ssid = "Agrivision_Field_Rover";
const char* ap_pass = "agrivision2026";

#define PIN_ENA 14
#define PIN_IN1 27
#define PIN_IN2 26
#define PIN_IN3 25
#define PIN_IN4 33
#define PIN_ENB 32

#define PIN_TRIG 12
#define PIN_ECHO 13
#define AUTO_STOP_DIST_CM 20.0

#define PIN_VBAT 34
#define VBAT_DIVIDER 2.0

#define PWM_CH_A 0
#define PWM_CH_B 1
#define PWM_FREQ 1000
#define PWM_RES 8

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

int currentPWM = 191; // 75% default throttle (191/255)
String driveState = "IDLE";
bool emergencyHalt = false;
float lastSonarDist = 42.4;

void setupMotors() {
  pinMode(PIN_IN1, OUTPUT);
  pinMode(PIN_IN2, OUTPUT);
  pinMode(PIN_IN3, OUTPUT);
  pinMode(PIN_IN4, OUTPUT);

  ledcSetup(PWM_CH_A, PWM_FREQ, PWM_RES);
  ledcSetup(PWM_CH_B, PWM_FREQ, PWM_RES);
  ledcAttachPin(PIN_ENA, PWM_CH_A);
  ledcAttachPin(PIN_ENB, PWM_CH_B);

  setThrottle(currentPWM);
  stopMotors();
}

void setThrottle(int pwm) {
  currentPWM = constrain(pwm, 0, 255);
  ledcWrite(PWM_CH_A, currentPWM);
  ledcWrite(PWM_CH_B, currentPWM);
}

void driveForward() {
  if (emergencyHalt) return;
  digitalWrite(PIN_IN1, HIGH);
  digitalWrite(PIN_IN2, LOW);
  digitalWrite(PIN_IN3, HIGH);
  digitalWrite(PIN_IN4, LOW);
  driveState = "FORWARD";
}

void driveReverse() {
  if (emergencyHalt) return;
  digitalWrite(PIN_IN1, LOW);
  digitalWrite(PIN_IN2, HIGH);
  digitalWrite(PIN_IN3, LOW);
  digitalWrite(PIN_IN4, HIGH);
  driveState = "REVERSE";
}

void steerLeft() {
  if (emergencyHalt) return;
  digitalWrite(PIN_IN1, LOW);
  digitalWrite(PIN_IN2, HIGH);
  digitalWrite(PIN_IN3, HIGH);
  digitalWrite(PIN_IN4, LOW);
  driveState = "LEFT";
}

void steerRight() {
  if (emergencyHalt) return;
  digitalWrite(PIN_IN1, HIGH);
  digitalWrite(PIN_IN2, LOW);
  digitalWrite(PIN_IN3, LOW);
  digitalWrite(PIN_IN4, HIGH);
  driveState = "RIGHT";
}

void stopMotors() {
  digitalWrite(PIN_IN1, LOW);
  digitalWrite(PIN_IN2, LOW);
  digitalWrite(PIN_IN3, LOW);
  digitalWrite(PIN_IN4, LOW);
  driveState = "STOP";
}

float readSonar() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  long duration = pulseIn(PIN_ECHO, HIGH, 26000);
  if (duration == 0) return 999.0;
  float dist = (duration * 0.0343) / 2.0;
  lastSonarDist = dist;

  if (dist < AUTO_STOP_DIST_CM && driveState == "FORWARD") {
    stopMotors();
    emergencyHalt = true;
    broadcastLog("HC-SR04", "Obstacle detected at " + String(dist, 1) + " cm. Auto-Stop engaged!", "danger");
  }
  return dist;
}

float readBattery() {
  int raw = analogRead(PIN_VBAT);
  return ((raw / 4095.0) * 3.3) * VBAT_DIVIDER;
}

void broadcastLog(String subsys, String msg, String type) {
  StaticJsonDocument<256> doc;
  doc["type"] = "log";
  doc["subsystem"] = subsys;
  doc["message"] = msg;
  doc["severity"] = type;
  doc["millis"] = millis();
  String out;
  serializeJson(doc, out);
  ws.textAll(out);
}

void onWsEvent(AsyncWebSocket *srv, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_DATA) {
    StaticJsonDocument<256> doc;
    if (!deserializeJson(doc, (char*)data)) {
      String cmd = doc["command"] | "";
      if (cmd == "FWD") driveForward();
      else if (cmd == "REV") driveReverse();
      else if (cmd == "LEFT") steerLeft();
      else if (cmd == "RIGHT") steerRight();
      else if (cmd == "STOP" || cmd == "COAST") stopMotors();
      else if (cmd == "HALT") { emergencyHalt = true; stopMotors(); }
      else if (cmd == "THROTTLE") setThrottle(doc["pwm"] | 191);
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  setupMotors();

  WiFi.softAP(ap_ssid, ap_pass);
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.begin();
  Serial.println("Agrivision Rover ESP32 Firmware Running.");
}

void loop() {
  ws.cleanupClients();
  static unsigned long lastTick = 0;
  if (millis() - lastTick >= 200) {
    lastTick = millis();
    float dist = readSonar();
    float vbat = readBattery();

    StaticJsonDocument<256> t;
    t["type"] = "telemetry";
    t["dist_cm"] = dist;
    t["vbat"] = vbat;
    t["pwm"] = currentPWM;
    t["state"] = driveState;
    t["rssi"] = WiFi.RSSI();
    String msg;
    serializeJson(t, msg);
    ws.textAll(msg);
  }
}
