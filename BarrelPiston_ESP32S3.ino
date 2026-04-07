#include <WiFi.h>
#include <PubSubClient.h>

// =====================================================
// IDENTITY / VERSION
// =====================================================
#define DEVICE_NAME "BarrelPiston"
#define FW_VERSION  "BarrelPiston v3.0.0-S3 (Continuous Mode)"

// =====================================================
// CONFIG
// =====================================================
const char* ssid         = "AlchemyGuest";
const char* password     = "VoodooVacation5601";

const char* mqtt_host    = "10.1.10.115";
const uint16_t mqtt_port = 1883;
const char* mqtt_user    = "";
const char* mqtt_pass    = "";

// MQTT Topics
const char* T_COMMAND    = "MermaidsTale/BarrelPiston/command";
const char* T_STATUS     = "MermaidsTale/BarrelPiston/status";
const char* T_STATE      = "MermaidsTale/BarrelPiston/state";
const char* T_SAFETY     = "MermaidsTale/BarrelPiston/safety";
const char* T_INFO       = "MermaidsTale/BarrelPiston/info";
const char* T_LASTERROR  = "MermaidsTale/BarrelPiston/lastError";
const char* T_UPTIME     = "MermaidsTale/BarrelPiston/uptime";
const char* T_VERSION    = "MermaidsTale/BarrelPiston/version";
const char* T_DEVICE     = "MermaidsTale/BarrelPiston/device";

// GPIO control topics
const char* T_GPIO2_CMD   = "MermaidsTale/BarrelPiston/GPIO2";
const char* T_GPIO4_CMD   = "MermaidsTale/BarrelPiston/GPIO4";
const char* T_GPIO2_STATE = "MermaidsTale/BarrelPiston/GPIO2/State";
const char* T_GPIO4_STATE = "MermaidsTale/BarrelPiston/GPIO4/State";

// Pins (ESP32-S3)
const int RELAY_EXTEND_PIN  = 18;
const int RELAY_RETRACT_PIN = 8;
const int LED_PIN           = -1;  // Disabled for ESP32-S3

// Timing
const uint32_t INTERLOCK_GAP_MS     = 120;      // Dead time between direction changes
const uint32_t STATUS_INTERVAL_MS   = 30000;    // Heartbeat/status publish interval
const uint32_t CMD_TIMEOUT_MS       = 120000;   // Auto-stop after 2 min of no commands
const uint32_t WATCHDOG_REBOOT_MS   = 600000;   // Reboot after 10 min inactivity
const uint32_t BACKOFF_MAX_MS       = 30000;    // Max reconnect backoff

// =====================================================
// GLOBALS
// =====================================================
WiFiClient wifi;
PubSubClient mqtt(wifi);

enum class PistonState : uint8_t {
  STOPPED,
  EXTENDING,
  RETRACTING,
  SAFETY
};

PistonState state = PistonState::STOPPED;

bool safety_latched = false;
bool relay_extend_intended = false;
bool relay_retract_intended = false;
bool gpio2_output = false;
bool gpio4_output = false;

uint32_t last_cmd_ms = 0;
uint32_t last_activity_ms = 0;
uint32_t last_status_publish_ms = 0;
uint32_t next_wifi_retry_ms = 0;
uint32_t next_mqtt_retry_ms = 0;
uint32_t wifi_backoff_ms = 1000;
uint32_t mqtt_backoff_ms = 1000;

char last_error[96] = "NONE";

// =====================================================
// LED BLINKER (disabled when LED_PIN < 0)
// =====================================================
struct Blinker {
  uint32_t interval_ms = 0;
  uint32_t last_ms = 0;
  bool level = false;

  void set(uint32_t ms) {
    if (LED_PIN < 0) return;
    interval_ms = ms;
    last_ms = millis();
  }

  void tick() {
    if (LED_PIN < 0 || !interval_ms) return;
    uint32_t now = millis();
    if (now - last_ms >= interval_ms) {
      last_ms = now;
      level = !level;
      digitalWrite(LED_PIN, level);
    }
  }

  void solid(bool on) {
    if (LED_PIN < 0) return;
    interval_ms = 0;
    level = on;
    digitalWrite(LED_PIN, on);
  }
} led;

// =====================================================
// HELPERS
// =====================================================
bool mqttReady() {
  return mqtt.connected();
}

void publishRetained(const char* topic, const char* payload) {
  if (mqttReady()) mqtt.publish(topic, payload, true);
}

void publishLive(const char* topic, const char* payload) {
  if (mqttReady()) mqtt.publish(topic, payload, false);
}

bool tokenEquals(const char* a, const char* b) {
  return strcasecmp(a, b) == 0;
}

bool parseTruth(const char* s) {
  return tokenEquals(s, "1") || tokenEquals(s, "TRUE") || tokenEquals(s, "ON") || tokenEquals(s, "HIGH");
}

bool parseFalse(const char* s) {
  return tokenEquals(s, "0") || tokenEquals(s, "FALSE") || tokenEquals(s, "OFF") || tokenEquals(s, "LOW");
}

const char* stateToString(PistonState s) {
  switch (s) {
    case PistonState::STOPPED:    return "STOPPED";
    case PistonState::EXTENDING:  return "EXTENDING";
    case PistonState::RETRACTING: return "RETRACTING";
    case PistonState::SAFETY:     return "SAFETY";
    default:                      return "UNKNOWN";
  }
}

void setLastError(const char* msg) {
  strncpy(last_error, msg, sizeof(last_error) - 1);
  last_error[sizeof(last_error) - 1] = '\0';
  publishRetained(T_LASTERROR, last_error);
}

void setRelays(bool extend_on, bool retract_on) {
  // Active LOW relays - LOW = ON, HIGH = OFF
  digitalWrite(RELAY_EXTEND_PIN,  extend_on  ? LOW : HIGH);
  digitalWrite(RELAY_RETRACT_PIN, retract_on ? LOW : HIGH);

  relay_extend_intended  = extend_on;
  relay_retract_intended = retract_on;
}

void allRelaysOff() {
  setRelays(false, false);
}

void publishFullStatus() {
  char info[256];
  char uptime[32];

  snprintf(uptime, sizeof(uptime), "%lu", millis() / 1000UL);

  publishRetained(T_DEVICE, DEVICE_NAME);
  publishRetained(T_VERSION, FW_VERSION);
  publishRetained(T_STATUS, mqtt.connected() ? "ONLINE" : "OFFLINE");
  publishRetained(T_STATE, stateToString(state));
  publishRetained(T_SAFETY, safety_latched ? "LATCHED" : "CLEAR");
  publishRetained(T_LASTERROR, last_error);
  publishRetained(T_UPTIME, uptime);

  snprintf(
    info,
    sizeof(info),
    "%s | Device=%s | IP=%s | State=%s | Safety=%s | ExtendRelay=%s | RetractRelay=%s | RSSI=%d",
    FW_VERSION,
    DEVICE_NAME,
    WiFi.localIP().toString().c_str(),
    stateToString(state),
    safety_latched ? "LATCHED" : "CLEAR",
    relay_extend_intended ? "ON" : "OFF",
    relay_retract_intended ? "ON" : "OFF",
    WiFi.RSSI()
  );
  publishRetained(T_INFO, info);
}

void publishGpioState(uint8_t pin, const char* topicState) {
  int v = digitalRead(pin);
  publishRetained(topicState, v ? "HIGH" : "LOW");
}

void handleGpioCmd(uint8_t pin, bool& promoted, const char* topicState, const char* payload) {
  if (tokenEquals(payload, "READ")) {
    publishGpioState(pin, topicState);
    return;
  }

  if (!promoted) {
    pinMode(pin, OUTPUT);
    promoted = true;
  }

  if (parseTruth(payload)) {
    digitalWrite(pin, HIGH);
  } else if (parseFalse(payload)) {
    digitalWrite(pin, LOW);
  } else {
    return;
  }

  publishGpioState(pin, topicState);
  last_activity_ms = millis();
}

// =====================================================
// STATE CONTROL (Continuous Mode - stays until changed)
// =====================================================
void toStopped(const char* reason) {
  allRelaysOff();
  state = PistonState::STOPPED;

  if (reason && !tokenEquals(reason, "NONE")) {
    setLastError(reason);
  }

  publishRetained(T_STATE, "STOPPED");
  publishRetained(T_SAFETY, safety_latched ? "LATCHED" : "CLEAR");
  Serial.printf("STOPPED: %s\n", reason ? reason : "normal");
}

void enterSafety(const char* reason) {
  allRelaysOff();
  state = PistonState::SAFETY;
  safety_latched = true;

  setLastError(reason);
  publishRetained(T_STATE, "SAFETY");
  publishRetained(T_SAFETY, "LATCHED");
  led.set(80);

  Serial.printf("SAFETY: %s\n", reason);
}

void startExtend() {
  if (safety_latched) {
    enterSafety("EXTEND_BLOCKED_SAFETY_LATCHED");
    return;
  }

  // If retracting, stop first with interlock gap
  if (state == PistonState::RETRACTING) {
    allRelaysOff();
    delay(INTERLOCK_GAP_MS);
  }

  setRelays(true, false);
  state = PistonState::EXTENDING;

  setLastError("NONE");
  publishRetained(T_STATE, "EXTENDING");
  publishRetained(T_SAFETY, "CLEAR");
  Serial.println("State: EXTENDING (continuous)");
}

void startRetract() {
  if (safety_latched) {
    enterSafety("RETRACT_BLOCKED_SAFETY_LATCHED");
    return;
  }

  // If extending, stop first with interlock gap
  if (state == PistonState::EXTENDING) {
    allRelaysOff();
    delay(INTERLOCK_GAP_MS);
  }

  setRelays(false, true);
  state = PistonState::RETRACTING;

  setLastError("NONE");
  publishRetained(T_STATE, "RETRACTING");
  publishRetained(T_SAFETY, "CLEAR");
  Serial.println("State: RETRACTING (continuous)");
}

// =====================================================
// COMMANDS
// =====================================================
void handlePrimaryCommand(const char* msg) {
  last_cmd_ms = millis();
  last_activity_ms = last_cmd_ms;
  Serial.printf("COMMAND: %s\n", msg);

  if (tokenEquals(msg, "PING")) {
    publishLive(T_INFO, "PONG");
    return;
  }

  if (tokenEquals(msg, "STATUS")) {
    publishFullStatus();
    return;
  }

  if (tokenEquals(msg, "RESET")) {
    safety_latched = false;
    allRelaysOff();
    state = PistonState::STOPPED;
    setLastError("NONE");
    publishRetained(T_SAFETY, "CLEAR");
    publishRetained(T_STATE, "STOPPED");
    publishFullStatus();
    led.solid(true);
    Serial.println("Manual reset complete");
    return;
  }

  if (tokenEquals(msg, "STOP")) {
    toStopped("STOP_COMMAND");
    return;
  }

  if (tokenEquals(msg, "EXTEND")) {
    startExtend();
    return;
  }

  if (tokenEquals(msg, "RETRACT")) {
    startRetract();
    return;
  }

  setLastError("UNKNOWN_COMMAND");
  publishLive(T_INFO, "ERROR: UNKNOWN_COMMAND");
}

void handleCommand(const char* topic, const char* msg) {
  if (!strcmp(topic, T_COMMAND)) {
    handlePrimaryCommand(msg);
    return;
  }

  if (!strcmp(topic, T_GPIO2_CMD)) {
    handleGpioCmd(2, gpio2_output, T_GPIO2_STATE, msg);
    return;
  }

  if (!strcmp(topic, T_GPIO4_CMD)) {
    handleGpioCmd(4, gpio4_output, T_GPIO4_STATE, msg);
    return;
  }
}

// =====================================================
// MQTT
// =====================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  static char buf[128];
  static char topicBuf[128];

  unsigned int safeTopicLen = min((unsigned int)strlen(topic), (unsigned int)(sizeof(topicBuf) - 1));
  memcpy(topicBuf, topic, safeTopicLen);
  topicBuf[safeTopicLen] = '\0';

  length = min(length, (unsigned int)(sizeof(buf) - 1));
  memcpy(buf, payload, length);
  buf[length] = '\0';

  Serial.printf("MQTT [%s] %s\n", topicBuf, buf);
  handleCommand(topicBuf, buf);
}

void mqttConnectIfNeeded() {
  if (mqtt.connected()) return;

  uint32_t now = millis();
  if (now < next_mqtt_retry_ms) return;

  Serial.println("MQTT: connecting...");
  mqtt.setServer(mqtt_host, mqtt_port);
  mqtt.setCallback(mqttCallback);

  String cid = String("ESP32S3-BarrelPiston-") + String((uint32_t)ESP.getEfuseMac(), HEX);

  bool ok;
  if (strlen(mqtt_user) == 0) {
    ok = mqtt.connect(cid.c_str(), T_STATUS, 1, true, "OFFLINE");
  } else {
    ok = mqtt.connect(cid.c_str(), mqtt_user, mqtt_pass, T_STATUS, 1, true, "OFFLINE");
  }

  if (ok) {
    Serial.println("MQTT: connected");
    mqtt_backoff_ms = 1000;

    mqtt.subscribe(T_COMMAND);
    mqtt.subscribe(T_GPIO2_CMD);
    mqtt.subscribe(T_GPIO4_CMD);

    publishRetained(T_DEVICE, DEVICE_NAME);
    publishRetained(T_VERSION, FW_VERSION);
    publishRetained(T_STATUS, "ONLINE");
    publishRetained(T_STATE, stateToString(state));
    publishRetained(T_SAFETY, safety_latched ? "LATCHED" : "CLEAR");
    publishRetained(T_LASTERROR, last_error);

    char info[128];
    snprintf(info, sizeof(info), "%s Ready - IP: %s", DEVICE_NAME, WiFi.localIP().toString().c_str());
    publishRetained(T_INFO, info);

    publishFullStatus();
    led.solid(true);
  } else {
    int rc = mqtt.state();
    Serial.printf("MQTT: failed rc=%d\n", rc);
    next_mqtt_retry_ms = now + mqtt_backoff_ms;
    mqtt_backoff_ms = min<uint32_t>(mqtt_backoff_ms * 2, BACKOFF_MAX_MS);
    led.set(400);
  }
}

// =====================================================
// WIFI
// =====================================================
void onArduinoEvent(arduino_event_t* event) {
  switch (event->event_id) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("WiFi: connected");
      break;

    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("WiFi: IP ");
      Serial.println(WiFi.localIP());
      wifi_backoff_ms = 1000;
      next_mqtt_retry_ms = 0;
      break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("WiFi: disconnected");
      led.set(250);
      break;

    default:
      break;
  }
}

void wifiEnsureConnected() {
  if (WiFi.status() == WL_CONNECTED) return;

  uint32_t now = millis();
  if (now < next_wifi_retry_ms) return;

  Serial.println("WiFi: (re)connecting...");
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  next_wifi_retry_ms = now + wifi_backoff_ms;
  wifi_backoff_ms = min<uint32_t>(wifi_backoff_ms * 2, BACKOFF_MAX_MS);
}

// =====================================================
// SETUP / LOOP
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("\n" FW_VERSION);
  Serial.println("Starting ESP32-S3 Barrel Piston Controller...");

  if (LED_PIN >= 0) {
    pinMode(LED_PIN, OUTPUT);
    led.set(300);
  }

  pinMode(RELAY_EXTEND_PIN, OUTPUT);
  pinMode(RELAY_RETRACT_PIN, OUTPUT);
  allRelaysOff();

  setLastError("NONE");

  WiFi.onEvent(onArduinoEvent);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  last_cmd_ms = millis();
  last_activity_ms = millis();
}

void loop() {
  led.tick();

  wifiEnsureConnected();

  if (WiFi.status() == WL_CONNECTED) {
    mqttConnectIfNeeded();
  }

  if (mqtt.connected()) {
    mqtt.loop();
  }

  // Safety check - both relays should never be active
  if (relay_extend_intended && relay_retract_intended) {
    enterSafety("HARDWARE_BOTH_RELAYS_ACTIVE");
  }

  uint32_t now = millis();

  // Auto-stop after command timeout (continuous mode safety)
  if ((state == PistonState::EXTENDING || state == PistonState::RETRACTING) &&
      (now - last_cmd_ms > CMD_TIMEOUT_MS)) {
    toStopped("TIMEOUT_AUTO_STOP");
    Serial.println("Auto-stopped after command timeout");
  }

  // Periodic status heartbeat
  if (mqtt.connected() && (now - last_status_publish_ms >= STATUS_INTERVAL_MS)) {
    publishFullStatus();
    last_status_publish_ms = now;
  }

  // Watchdog: reboot after long inactivity
  if (last_activity_ms > 0 && (now - last_activity_ms > WATCHDOG_REBOOT_MS)) {
    Serial.println("Watchdog: restarting...");
    ESP.restart();
  }

  delay(5);
}
