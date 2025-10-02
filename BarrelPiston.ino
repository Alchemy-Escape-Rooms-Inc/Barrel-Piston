#include <WiFi.h>
#include <PubSubClient.h>

// ===========================
// CONFIG
// ===========================
const char* ssid       = "AlchemyGuest";
const char* password   = "VoodooVacation5601";

const char* mqtt_host  = "10.1.10.115";
const uint16_t mqtt_port = 1883;
const char* mqtt_user  = "YOUR_MQTT_USERNAME";   // "" if none
const char* mqtt_pass  = "YOUR_MQTT_PASSWORD";   // "" if none

// Topics
const char* T_ENGAGE      = "MermaidsTale/BarrelPiston/Engaged";
const char* T_RETRACT     = "MermaidsTale/BarrelPiston/Retract";
const char* T_STATUS      = "MermaidsTale/BarrelPiston/Status";
const char* T_SAFETY      = "MermaidsTale/BarrelPiston/Safety";
const char* T_INFO        = "MermaidsTale/BarrelPiston/DeviceInfo";

// GPIO2/4 control topics (+ state echoes)
const char* T_GPIO2_CMD   = "MermaidsTale/BarrelPiston/GPIO2";
const char* T_GPIO4_CMD   = "MermaidsTale/BarrelPiston/GPIO4";
const char* T_GPIO2_STATE = "MermaidsTale/BarrelPiston/GPIO2/State";
const char* T_GPIO4_STATE = "MermaidsTale/BarrelPiston/GPIO4/State";

// Pins (avoid strap pins for RELAYS; move status LED off GPIO2)
const int RELAY_ENGAGE_PIN  = 16;    // GPIO16
const int RELAY_RETRACT_PIN = 17;    // GPIO17
const int LED_PIN           = 25;    // Status LED

// Timing
const uint32_t CMD_TIMEOUT_MS      = 5000;     // auto-stop piston after no command
const uint32_t WATCHDOG_REBOOT_MS  = 600000;   // reboot after 10 min inactivity
const uint32_t INTERLOCK_GAP_MS    = 120;      // gap when swapping direction
const uint32_t BACKOFF_MAX_MS      = 30000;

// ===========================
// GLOBALS
// ===========================
WiFiClient wifi;
PubSubClient mqtt(wifi);

enum class PistonState : uint8_t { STOPPED, ENGAGING, RETRACTING, SAFETY };
PistonState state = PistonState::STOPPED;

bool safety_latched = false;
uint32_t last_cmd_ms = 0;
uint32_t last_activity_ms = 0;

// Track what the controller WANTS to do (not what MQTT sends)
bool want_engage = false;
bool want_retract = false;

// Track whether we've promoted GPIO2/4 to OUTPUT yet
bool gpio2_output = false;
bool gpio4_output = false;

// Track the actual intended relay states (more reliable than digitalRead on OUTPUT pins)
bool relay_engage_intended = false;
bool relay_retract_intended = false;

// LED blinker (non-blocking)
struct Blinker {
  uint32_t interval_ms = 0, last_ms = 0;
  bool level = false;
  void set(uint32_t ms){ interval_ms = ms; last_ms = millis(); }
  void tick(){
    if(!interval_ms) return;
    uint32_t now=millis();
    if(now-last_ms>=interval_ms){ last_ms=now; level=!level; digitalWrite(LED_PIN, level); }
  }
  void solid(bool on){ interval_ms=0; digitalWrite(LED_PIN,on); level=on; }
} led;

uint32_t next_wifi_retry_ms = 0, next_mqtt_retry_ms = 0;
uint32_t wifi_backoff_ms = 1000, mqtt_backoff_ms = 1000;

// ===========================
// UTIL
// ===========================
void setRelays(bool engage_on, bool retract_on) {
  // If your relay board is active-LOW, invert these writes.
  digitalWrite(RELAY_ENGAGE_PIN, engage_on ? HIGH : LOW);
  digitalWrite(RELAY_RETRACT_PIN, retract_on ? HIGH : LOW);
}

void publishStatus(const char* s, bool retained=false){ mqtt.publish(T_STATUS, s, retained); }
void publishSafety(const char* s, bool retained=false){ mqtt.publish(T_SAFETY, s, retained); }

void toStopped(const char* reason=nullptr) {
  state = PistonState::STOPPED;
  safety_latched = false;
  want_engage = false;
  want_retract = false;
  setRelays(false, false);
  publishStatus("STOPPED");
  if (reason) publishSafety(reason);
  Serial.printf("State: STOPPED (%s)\n", reason ? reason : "manual");
}

void toEngaging() {
  // Safety: ensure retract is off first
  setRelays(false, false);
  delay(INTERLOCK_GAP_MS);
  
  setRelays(true, false);
  safety_latched = false;
  state = PistonState::ENGAGING;
  publishStatus("ENGAGING");
  Serial.println("State: ENGAGING");
}

void toRetracting(){
  // Safety: ensure engage is off first
  setRelays(false, false);
  delay(INTERLOCK_GAP_MS);
  
  setRelays(false, true);
  safety_latched = false;
  state = PistonState::RETRACTING;
  publishStatus("RETRACTING");
  Serial.println("State: RETRACTING");
}

void safetyShutdown(const char* why) {
  if(!safety_latched){
    safety_latched=true;
    publishSafety("EMERGENCY_STOP");
    publishStatus("SAFETY_SHUTDOWN");
  }
  setRelays(false,false);
  state=PistonState::SAFETY;
  want_engage = false;
  want_retract = false;
  led.set(80); // fast blink
  publishSafety(why);
  Serial.printf("SAFETY SHUTDOWN: %s\n", why);
}

bool parseTruth(const char* s){
  return (!strcasecmp(s,"1")||!strcasecmp(s,"true")||!strcasecmp(s,"on")||!strcasecmp(s,"high"));
}
bool parseFalse(const char* s){
  return (!strcasecmp(s,"0")||!strcasecmp(s,"false")||!strcasecmp(s,"off")||!strcasecmp(s,"low"));
}

void publishGpioState(uint8_t pin, const char* topicState){
  int v = digitalRead(pin);
  mqtt.publish(topicState, v ? "HIGH" : "LOW", true);
}

void handleGpioCmd(uint8_t pin, bool& promoted, const char* topicState, const char* payload){
  if (!strcasecmp(payload, "READ")) {
    publishGpioState(pin, topicState);
    return;
  }
  if (!promoted) {
    pinMode(pin, OUTPUT);   // promote to OUTPUT only when commanded
    promoted = true;
  }
  if (parseTruth(payload)) {
    digitalWrite(pin, HIGH);
  } else if (parseFalse(payload)) {
    digitalWrite(pin, LOW);
  } else {
    // ignore unknown tokens
    return;
  }
  publishGpioState(pin, topicState);
  last_cmd_ms = last_activity_ms = millis();
}

// NEW: Process the desired state and handle mutual exclusion
void processDesiredState() {
  // Safety: never allow both to be true
  if (want_engage && want_retract) {
    safetyShutdown("BOTH_COMMANDS_ACTIVE");
    return;
  }
  
  // If in safety mode, ignore commands until reset
  if (state == PistonState::SAFETY) {
    return;
  }
  
  // Process the desired action
  if (want_engage && !want_retract) {
    if (state != PistonState::ENGAGING) {
      toEngaging();
    }
  }
  else if (want_retract && !want_engage) {
    if (state != PistonState::RETRACTING) {
      toRetracting();
    }
  }
  else {
    // Both false or safety condition - stop
    if (state == PistonState::ENGAGING || state == PistonState::RETRACTING) {
      toStopped("COMMAND_CLEARED");
    }
  }
}

void handleCommand(const char* topic, const char* msg) {
  last_cmd_ms = millis();
  last_activity_ms = last_cmd_ms;
  
  Serial.printf("Command received - Topic: %s, Message: %s\n", topic, msg);

  if (!strcmp(topic, T_ENGAGE)) {
    want_engage = parseTruth(msg);
    Serial.printf("want_engage set to: %s\n", want_engage ? "TRUE" : "FALSE");
    
    // Read and report current relay pin states
    bool engage_pin_state = digitalRead(RELAY_ENGAGE_PIN);
    bool retract_pin_state = digitalRead(RELAY_RETRACT_PIN);
    Serial.printf("Relay pins after ENGAGE command - Pin %d: %s, Pin %d: %s\n", 
                  RELAY_ENGAGE_PIN, engage_pin_state ? "HIGH" : "LOW",
                  RELAY_RETRACT_PIN, retract_pin_state ? "HIGH" : "LOW");
    
  } else if (!strcmp(topic, T_RETRACT)) {
    want_retract = parseTruth(msg);
    Serial.printf("want_retract set to: %s\n", want_retract ? "TRUE" : "FALSE");
    
    // Read and report current relay pin states
    bool engage_pin_state = digitalRead(RELAY_ENGAGE_PIN);
    bool retract_pin_state = digitalRead(RELAY_RETRACT_PIN);
    Serial.printf("Relay pins after RETRACT command - Pin %d: %s, Pin %d: %s\n", 
                  RELAY_ENGAGE_PIN, engage_pin_state ? "HIGH" : "LOW",
                  RELAY_RETRACT_PIN, retract_pin_state ? "HIGH" : "LOW");
    
  } else if (!strcmp(topic, T_GPIO2_CMD)) {
    handleGpioCmd(/*pin*/2, gpio2_output, T_GPIO2_STATE, msg);
    return; // Don't process state for GPIO commands
    
  } else if (!strcmp(topic, T_GPIO4_CMD)) {
    handleGpioCmd(/*pin*/4, gpio4_output, T_GPIO4_STATE, msg);
    return; // Don't process state for GPIO commands
  }
  
  // Process the new desired state
  processDesiredState();
}

// ===========================
// MQTT
// ===========================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  static char buf[128];
  length = min(length, (unsigned int)(sizeof(buf)-1));
  memcpy(buf, payload, length); buf[length]='\0';
  Serial.printf("MQTT [%s] %s\n", topic, buf);
  handleCommand(topic, buf);
}

void mqttConnectIfNeeded() {
  if (mqtt.connected()) return;

  uint32_t now = millis();
  if (now < next_mqtt_retry_ms) return;

  Serial.println("MQTT: connecting...");
  mqtt.setServer(mqtt_host, mqtt_port);
  mqtt.setCallback(mqttCallback);

  // LWT so broker marks us OFFLINE on unexpected drop
  String cid = String("ESP32-Piston-") + String((uint32_t)ESP.getEfuseMac(), HEX);
  bool ok = (strlen(mqtt_user)==0)
    ? mqtt.connect(cid.c_str(), T_STATUS, 1, true, "OFFLINE")
    : mqtt.connect(cid.c_str(), mqtt_user, mqtt_pass, T_STATUS, 1, true, "OFFLINE");

  if (ok) {
    Serial.println("MQTT: connected");
    mqtt_backoff_ms = 1000;

    mqtt.subscribe(T_ENGAGE);
    mqtt.subscribe(T_RETRACT);
    mqtt.subscribe(T_GPIO2_CMD);
    mqtt.subscribe(T_GPIO4_CMD);

    publishStatus("ONLINE", true);
    publishSafety("SYSTEM_READY", true);

    String info = "ESP32 Ready - IP: " + WiFi.localIP().toString();
    mqtt.publish(T_INFO, info.c_str(), true);

    // Publish initial GPIO states (INPUT default; only promote on set)
    pinMode(2, INPUT);
    pinMode(4, INPUT);
    publishGpioState(2, T_GPIO2_STATE);
    publishGpioState(4, T_GPIO4_STATE);

    led.solid(true); // solid when fully online
  } else {
    int rc = mqtt.state();
    Serial.printf("MQTT: failed rc=%d\n", rc);
    next_mqtt_retry_ms = now + mqtt_backoff_ms;
    mqtt_backoff_ms = min<uint32_t>(mqtt_backoff_ms*2, BACKOFF_MAX_MS);
    led.set(400); // slow blink while disconnected
  }
}

// ===========================
// WIFI (ESP32 core 3.3.0 Arduino events)
// ===========================
void onArduinoEvent(arduino_event_t* event) {
  switch (event->event_id) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("WiFi: connected");
      break;

    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("WiFi: IP ");
      Serial.println(WiFi.localIP());
      next_mqtt_retry_ms = 0;   // allow immediate MQTT try
      break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("WiFi: disconnected");
      led.set(250);             // blink while reconnecting
      break;

    default:
      break;
  }
}

void wifiEnsureConnected() {
  if (WiFi.status() == WL_CONNECTED) return;

  static uint32_t last_attempt_ms = 0;
  uint32_t now = millis();
  if (now - last_attempt_ms < wifi_backoff_ms) return;

  Serial.println("WiFi: (re)connecting...");
  WiFi.disconnect(true,true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid,password);

  last_attempt_ms = now;
  wifi_backoff_ms = min<uint32_t>(wifi_backoff_ms*2, BACKOFF_MAX_MS);
}

// ===========================
// SETUP/LOOP
// ===========================
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("\nStarting ESP32 Piston Controller...");

  pinMode(LED_PIN, OUTPUT);
  led.set(300); // searching blink

  // Relays default off
  pinMode(RELAY_ENGAGE_PIN, OUTPUT);
  pinMode(RELAY_RETRACT_PIN, OUTPUT);
  setRelays(false,false);

  // Keep GPIO2/4 INPUT until commanded
  pinMode(2, INPUT);
  pinMode(4, INPUT);

  WiFi.onEvent(onArduinoEvent);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  last_cmd_ms = last_activity_ms = millis();
}

void loop() {
  led.tick();

  wifiEnsureConnected();
  if (WiFi.status() == WL_CONNECTED) mqttConnectIfNeeded();
  if (mqtt.connected()) mqtt.loop();

  // Hardware safety check - use intended states instead of unreliable digitalRead
  if (relay_engage_intended && relay_retract_intended) {
    safetyShutdown("HARDWARE_BOTH_ACTIVE");
  }

  uint32_t now = millis();

  // Auto-stop after timeout while motion requested
  if ((state==PistonState::ENGAGING || state==PistonState::RETRACTING) &&
      (now - last_cmd_ms > CMD_TIMEOUT_MS)) {
    want_engage = false;
    want_retract = false;
    toStopped("TIMEOUT_STOP");
  }

  // Watchdog: reboot after long inactivity (no commands)
  if (last_activity_ms>0 && (now - last_activity_ms > WATCHDOG_REBOOT_MS)) {
    Serial.println("Watchdog: restarting...");
    ESP.restart();
  }

  delay(5); // light cadence
}