/*
 * ============================================================
 *  Smart Air Conditioner  -  ESP32 + DHT11 + MQTT + Node-RED
 *  IoT course (ENP) - Wokwi simulation
 * ============================================================
 *  Sensor   : DHT11 (Wokwi only offers the DHT22 part, which uses
 *             the same 1-wire protocol -> change DHT_TYPE to DHT11
 *             on real hardware, nothing else changes)
 *  Actuators: LED blue  = compressor COOLING
 *             LED red   = HEATING (heat pump)
 *             LED green = AC powered ON
 *             Servo     = swinging air louver (speed = fan speed)
 *             Buzzer    = "beep" when a command is received
 *  Local UI : LCD 16x2 I2C + push button (power ON/OFF)
 *  Network  : Wi-Fi -> MQTT broker (public.cloud.shiftr.io:1883)
 *
 *  MQTT topics (base = enp/zaki/ac/)
 *    PUBLISH   enp/zaki/ac/telemetry     JSON every 2 s
 *    PUBLISH   enp/zaki/ac/status        "online" (retained, LWT = "offline")
 *    SUBSCRIBE enp/zaki/ac/cmd/power     "ON" | "OFF" | "TOGGLE"
 *    SUBSCRIBE enp/zaki/ac/cmd/mode      "AUTO" | "COOL" | "HEAT" | "FAN"
 *    SUBSCRIBE enp/zaki/ac/cmd/setpoint  16 ... 30 (deg C)
 * ============================================================
 */
#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>

// ---------------- Network ----------------
const char* WIFI_SSID = "Wokwi-GUEST";   // Wokwi virtual access point
const char* WIFI_PASS = "";
const char* MQTT_HOST = "public.cloud.shiftr.io";   // free public broker
const int   MQTT_PORT = 1883;
const char* MQTT_USER = "public";
const char* MQTT_PASS = "public";

// ---------------- MQTT topics ----------------
#define BASE "enp/zaki/ac/"
const char* TOPIC_TELEMETRY = BASE "telemetry";
const char* TOPIC_STATUS    = BASE "status";
const char* TOPIC_CMD       = BASE "cmd/#";   // '#' = all command sub-topics

// ---------------- Pins ----------------
#define DHT_PIN     32
#define DHT_TYPE    DHT22      // DHT11 on the real board
#define LED_HEAT    25
#define LED_COOL    26
#define LED_POWER   27
#define SERVO_PIN   18
#define BUZZER_PIN  19
#define BTN_PIN     4

// ---------------- Objects ----------------
DHT dht(DHT_PIN, DHT_TYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo louver;
WiFiClient espClient;
PubSubClient mqtt(espClient);

// ---------------- AC state ----------------
bool   power    = true;
String mode     = "AUTO";      // AUTO | COOL | HEAT | FAN
float  setpoint = 24.0;        // target temperature (deg C)
const float HYST = 1.0;        // hysteresis band (avoids ON/OFF chattering)

float  temp = NAN, hum = NAN;
bool   cooling = false, heating = false;
int    fanSpeed = 0;           // 0 = off, 1 = low, 2 = medium, 3 = high
String state = "OFF";          // OFF | IDLE | COOLING | HEATING | FAN

// ---------------- Timers ----------------
unsigned long lastRead = 0, lastMqttTry = 0, lastSwing = 0, lastBtn = 0;
int  louverAngle = 90, louverDir = 1;
bool lastBtnLevel = HIGH;

// ============================================================
void beep() { tone(BUZZER_PIN, 2200, 60); }

// ------------------------------------------------------------
// Control logic: thermostat with hysteresis
// ------------------------------------------------------------
void updateControl() {
  if (!power || isnan(temp)) {
    cooling = heating = false;
    fanSpeed = 0;
    state = power ? "IDLE" : "OFF";
  } else if (mode == "FAN") {
    cooling = heating = false;
    fanSpeed = 2;
    state = "FAN";
  } else {
    bool canCool = (mode == "COOL" || mode == "AUTO");
    bool canHeat = (mode == "HEAT" || mode == "AUTO");

    // Cooling: starts above SP + HYST, stops when SP is reached
    if (!canCool)                               cooling = false;
    else if (!cooling && temp >= setpoint + HYST) cooling = true;
    else if ( cooling && temp <= setpoint)        cooling = false;

    // Heating: starts below SP - HYST, stops when SP is reached
    if (!canHeat)                               heating = false;
    else if (!heating && temp <= setpoint - HYST) heating = true;
    else if ( heating && temp >= setpoint)        heating = false;

    float gap = fabs(temp - setpoint);
    if (cooling || heating) fanSpeed = (gap < 2) ? 1 : (gap < 5 ? 2 : 3);
    else                    fanSpeed = 1;       // ventilation only
    state = cooling ? "COOLING" : (heating ? "HEATING" : "IDLE");
  }

  digitalWrite(LED_POWER, power);
  digitalWrite(LED_COOL, cooling);
  digitalWrite(LED_HEAT, heating);
}

// ------------------------------------------------------------
void updateLcd() {
  char l1[17], l2[17];
  if (isnan(temp)) snprintf(l1, 17, "Sensor error    ");
  else snprintf(l1, 17, "%4.1f%cC %2.0f%% %-4s ", temp, (char)223, hum, mode.c_str());
  if (!power) snprintf(l2, 17, "SP:%4.1f AC OFF ", setpoint);
  else        snprintf(l2, 17, "SP:%4.1f %-7s ", setpoint, state.c_str());
  lcd.setCursor(0, 0); lcd.print(l1);
  lcd.setCursor(0, 1); lcd.print(l2);
}

// ------------------------------------------------------------
void publishTelemetry() {
  if (!mqtt.connected() || isnan(temp)) return;   // nothing valid to send yet
  char json[200];
  snprintf(json, sizeof(json),
    "{\"temp\":%.1f,\"hum\":%.1f,\"setpoint\":%.1f,\"power\":\"%s\","
    "\"mode\":\"%s\",\"state\":\"%s\",\"fan\":%d}",
    isnan(temp) ? 0.0 : temp, isnan(hum) ? 0.0 : hum, setpoint,
    power ? "ON" : "OFF", mode.c_str(), state.c_str(), fanSpeed);
  mqtt.publish(TOPIC_TELEMETRY, json);
  Serial.printf("[PUB] %s -> %s\n", TOPIC_TELEMETRY, json);
}

void refreshAll() { updateControl(); updateLcd(); publishTelemetry(); }

// ------------------------------------------------------------
// MQTT callback: commands coming from Node-RED
// ------------------------------------------------------------
void onMqttMessage(char* topic, byte* payload, unsigned int len) {
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  msg.trim();
  String t = topic;
  Serial.printf("[SUB] %s <- %s\n", topic, msg.c_str());

  String up = msg; up.toUpperCase();
  if (t.endsWith("/power")) {
    if (up == "ON" || up == "TRUE" || up == "1")        power = true;
    else if (up == "OFF" || up == "FALSE" || up == "0") power = false;
    else if (up == "TOGGLE")                            power = !power;
  } else if (t.endsWith("/mode")) {
    if (up == "AUTO" || up == "COOL" || up == "HEAT" || up == "FAN") mode = up;
  } else if (t.endsWith("/setpoint")) {
    float v = msg.toFloat();
    if (v >= 16 && v <= 30) setpoint = v;
  } else return;

  beep();
  refreshAll();
}

// ------------------------------------------------------------
void connectWiFi() {
  lcd.clear(); lcd.print("Connecting WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
  while (WiFi.status() != WL_CONNECTED) { delay(250); Serial.print("."); }
  Serial.printf("\nWiFi OK, IP = %s\n", WiFi.localIP().toString().c_str());
}

void connectMqtt() {                       // non-blocking: 1 try every 3 s
  if (mqtt.connected() || millis() - lastMqttTry < 3000) return;
  lastMqttTry = millis();
  String id = "enp-ac-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.print("MQTT connecting... ");
  // connect(id, user, pass, willTopic, willQos, willRetain, willMessage)
  if (mqtt.connect(id.c_str(), MQTT_USER, MQTT_PASS, TOPIC_STATUS, 1, true, "offline")) {
    Serial.println("OK");
    mqtt.publish(TOPIC_STATUS, "online", true);
    mqtt.subscribe(TOPIC_CMD);
    publishTelemetry();
  } else {
    Serial.printf("failed, rc=%d\n", mqtt.state());
  }
}

// ------------------------------------------------------------
void handleButton() {                      // local power button
  bool lvl = digitalRead(BTN_PIN);
  if (lvl == LOW && lastBtnLevel == HIGH && millis() - lastBtn > 250) {
    lastBtn = millis();
    power = !power;
    beep();
    refreshAll();
  }
  lastBtnLevel = lvl;
}

void animateLouver() {                     // swing speed = fan speed
  if (millis() - lastSwing < 20) return;
  lastSwing = millis();
  if (fanSpeed == 0) {                     // AC off -> louver closed
    if (louverAngle != 0) { louverAngle = 0; louver.write(0); }
    return;
  }
  if (louverAngle < 45) louverAngle = 45;
  louverAngle += louverDir * fanSpeed;
  if (louverAngle >= 135) { louverAngle = 135; louverDir = -1; }
  if (louverAngle <= 45)  { louverAngle = 45;  louverDir = 1; }
  louver.write(louverAngle);
}

// ============================================================
void setup() {
  Serial.begin(115200);
  pinMode(LED_HEAT, OUTPUT);
  pinMode(LED_COOL, OUTPUT);
  pinMode(LED_POWER, OUTPUT);
  pinMode(BTN_PIN, INPUT_PULLUP);

  lcd.init(); lcd.backlight();
  lcd.print(" Smart AC  v1.0"); lcd.setCursor(0, 1); lcd.print(" ESP32 + MQTT");
  dht.begin();
  louver.attach(SERVO_PIN);
  louver.write(0);
  delay(1000);

  connectWiFi();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  lcd.clear();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  connectMqtt();
  mqtt.loop();
  handleButton();

  if (millis() - lastRead >= 2000) {       // DHT11: max 1 reading / s
    lastRead = millis();
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) { temp = t; hum = h; }
    refreshAll();
  }
  animateLouver();
}
