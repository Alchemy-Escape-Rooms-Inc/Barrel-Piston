#include <WiFi.h>
#include <PubSubClient.h>

// =====================================================
// IDENTITY / VERSION
// =====================================================
#define DEVICE_NAME "BarrelPiston"
#define FW_VERSION  "BarrelPiston v2.0.1-S3 (Pulse Mode)"

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

// Optional GPIO topics
const char* T_GPIO2_CMD   = "MermaidsTale/BarrelPiston/GPIO2";
const char* T_GPIO4_CMD   = "MermaidsTale/BarrelPiston/GPIO4";
const char* T_GPIO2_STATE = "MermaidsTale/BarrelPiston/GPIO2/State";
const char* T_GPIO4_STATE = "MermaidsTale/BarrelPiston/GPIO4/State";

// Pins (ESP32-S3 compatible)
const int RELAY_EXTEND_PIN  = 16;
const int RELAY_RETRACT_PIN = 17;
const int LED_PIN           = -1;  // Disabled for ESP32-S3

// Timing
const uint32_t PULSE_MS             = 350;      // Adjust if needed for your valve
const uint32_t INTERLOCK_GAP_MS     = 120;      // Dead time between directions
const uint32_t STATUS_INTERVAL_MS   = 30000;    // Heartbeat/status publish interval
const uint32_t WATCHDOG_REBOOT_MS   = 600000;   // Reboot after 10 min inactivity
const uint32_t BACKOFF_MAX_MS       = 30000;    // Max reconnect backoff

// =====================================================
// GLOBALS
// =====================================================
WiFiClient wifi;
PubSubClient mqtt(wifi);

enum class PistonState : uint8_t {
  IDLE,
  PULSE_EXTEND,
  PULSE_RETRACT,
  INTERLOCK,
  SAFETY
};

enum class PendingAction : uint8_t {
  NONE,
  EXTEND,
  RETRACT
};

PistonState state = PistonState::IDLE;
PendingAction pendingAction = PendingAction::NONE;

bool safety_latched = false;
bool relay_extend_intended = false;
bool relay_retract_intended = false;
bool gpio2_output = false;
bool gpio4_output = false;

uint32_t pulse_start_ms = 0;
uint32_t interlock_start_ms = 0;
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
    case PistonState::IDLE:          return "IDLE";
    case PistonState::PULSE_EXTEND:  return "PULSE_EXTEND";
    case PistonState::PULSE_RETRACT: return "PULSE_RETRACT";
    case PistonState::INTERLOCK:     return "INTERLOCK";
    case PistonState::SAFETY:        return "SAFETY";
    default:                         return "UNKNOWN";
  }
}

const char* pendingToString(PendingAction a) {
  switch (a) {
    case PendingAction::NONE:    return "NONE";
    case PendingAction::EXTEND:  return "EXTEND";
    case PendingAction::RETRACT: return "RETRACT";
    default:                     return "UNKNOWN";
  }
}

void setLastError(const char* msg) {
  strncpy(last_error, msg, sizeof(last_error) - 1);
  last_error[sizeof(last_error) - 1] = '\0';
  publishRetained(T_LASTERROR, last_error);
}

void setRelays(bool extend_on, bool retract_on) {
  digitalWrite(RELAY_EXTEND_PIN,  extend_on  ? HIGH : LOW);
  digitalWrite(RELAY_RETRACT_PIN, retract_on ? HIGH : LOW);

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
    "%s | Device=%s | IP=%s | State=%s | Safety=%s | Pending=%s | ExtendRelay=%s | RetractRelay=%s | RSSI=%d",
    FW_VERSION,
    DEVICE_NAME,
    WiFi.localIP().toString().c_str(),
    stateToString(state),
    safety_latched ? "LATCHED" : "CLEAR",
    pendingToString(pendingAction),
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
// STATE CONTROL
// =====================================================
void setIdle(const char* reason) {
  allRelaysOff();
  state = PistonState::IDLE;
  pendingAction = PendingAction::NONE;

  if (!tokenEquals(reason, "NONE")) {
    setLastError(reason);
  }

  publishRetained(T_STATE, "IDLE");
  publishRetained(T_SAFETY, safety_latched ? "LATCHED" : "CLEAR");
  Serial.printf("IDLE: %s\n", reason);
}

void enterSafety(const char* reason) {
  allRelaysOff();
  state = PistonState::SAFETY;
  pendingAction = PendingAction::NONE;
  safety_latched = true;

  setLastError(reason);
  publishRetained(T_STATE, "SAFETY");
  publishRetained(T_SAFETY, "LATCHED");
  led.set(80);

  Serial.printf("SAFETY: %s\n", reason);
}

void beginInterlock(PendingAction nextAction, const char* reason) {
  allRelaysOff();
  state = PistonState::INTERLOCK;
  pendingAction = nextAction;
  interlock_start_ms = millis();

  setLastError(reason);
  publishRetained(T_STATE, "INTERLOCK");
  Serial.printf("INTERLOCK -> %s (%s)\n", pendingToString(nextAction), reason);
}

void startPulseExtend() {
  if (safety_latched) {
    enterSafety("EXTEND_BLOCKED_SAFETY_LATCHED");
    return;
  }

  allRelaysOff();
  setRelays(true, false);
  state = PistonState::PULSE_EXTEND;
  pendingAction = PendingAction::NONE;
  pulse_start_ms = millis();

  setLastError("NONE");
  publishRetained(T_STATE, "PULSE_EXTEND");
  publishRetained(T_SAFETY, "CLEAR");
  Serial.println("Pulse: EXTEND");
}

void startPulseRetract() {
  if (safety_latched) {
    enterSafety("RETRACT_BLOCKED_SAFETY_LATCHED");
    return;
  }

  allRelaysOff();
  setRelays(false, true);
  state = PistonState::PULSE_RETRACT;
  pendingAction = PendingAction::NONE;
  pulse_start_ms = millis();

  setLastError("NONE");
  publishRetained(T_STATE, "PULSE_RETRACT");
  publishRetained(T_SAFETY, "CLEAR");
  Serial.println("Pulse: RETRACT");
}

void processMotionState() {
  uint32_t now = millis();

  if (state == PistonState::PULSE_EXTEND || state == PistonState::PULSE_RETRACT) {
    if (now - pulse_start_ms >= PULSE_MS) {
      allRelaysOff();
      state = PistonState::IDLE;
      publishRetained(T_STATE, "IDLE");
      Serial.println("Pulse complete -> IDLE");
    }
  }

  if (state == PistonState::INTERLOCK) {
    if (now - interlock_start_ms >= INTERLOCK_GAP_MS) {
      PendingAction action = pendingAction;
      pendingAction = PendingAction::NONE;

      if (action == PendingAction::EXTEND) {
        startPulseExtend();
      } else if (action == PendingAction::RETRACT) {
        startPulseRetract();
      } else {
        setIdle("INTERLOCK_COMPLETE");
      }
    }
  }
}

// =====================================================
// COMMANDS
// =====================================================
void handlePrimaryCommand(const char* msg) {
  last_activity_ms = millis();
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
    state = PistonState::IDLE;
    pendingAction = PendingAction::NONE;
    setLastError("NONE");
    publishRetained(T_SAFETY, "CLEAR");
    publishRetained(T_STATE, "IDLE");
    publishFullStatus();
    led.solid(true);
    Serial.println("Manual reset complete");
    return;
  }

  if (tokenEquals(msg, "STOP")) {
    allRelaysOff();
    state = PistonState::IDLE;
    pendingAction = PendingAction::NONE;
    publishRetained(T_STATE, "IDLE");
    Serial.println("STOP -> IDLE");
    return;
  }

  if (tokenEquals(msg, "EXTEND")) {
    if (safety_latched) {
      enterSafety("EXTEND_WHILE_SAFETY_LATCHED");
      return;
    }

    if (state == PistonState::PULSE_RETRACT) {
      beginInterlock(PendingAction::EXTEND, "SWAP_RETRACT_TO_EXTEND");
    } else if (state == PistonState::INTERLOCK) {
      pendingAction = PendingAction::EXTEND;
    } else {
      startPulseExtend();
    }
    return;
  }

  if (tokenEquals(msg, "RETRACT")) {
    if (safety_latched) {
      enterSafety("RETRACT_WHILE_SAFETY_LATCHED");
      return;
    }

    if (state == PistonState::PULSE_EXTEND) {
      beginInterlock(PendingAction::RETRACT, "SWAP_EXTEND_TO_RETRACT");
    } else if (state == PistonState::INTERLOCK) {
      pendingAction = PendingAction::RETRACT;
    } else {
      startPulseRetract();
    }
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

    pinMode(2, INPUT);
    pinMode(4, INPUT);
    publishGpioState(2, T_GPIO2_STATE);
    publishGpioState(4, T_GPIO4_STATE);

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
  Serial.println("\nStarting ESP32-S3 Barrel Piston Pulse Controller...");

  if (LED_PIN >= 0) {
    pinMode(LED_PIN, OUTPUT);
    led.set(300);
  }

  pinMode(RELAY_EXTEND_PIN, OUTPUT);
  pinMode(RELAY_RETRACT_PIN, OUTPUT);
  allRelaysOff();

  pinMode(2, INPUT);
  pinMode(4, INPUT);

  setLastError("NONE");

  WiFi.onEvent(onArduinoEvent);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

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

  // Impossible output state check
  if (relay_extend_intended && relay_retract_intended) {
    enterSafety("HARDWARE_BOTH_RELAYS_ACTIVE");
  }

  processMotionState();

  uint32_t now = millis();

  if (mqtt.connected() && (now - last_status_publish_ms >= STATUS_INTERVAL_MS)) {
    publishFullStatus();
    last_status_publish_ms = now;
  }

  if (last_activity_ms > 0 && (now - last_activity_ms > WATCHDOG_REBOOT_MS)) {
    Serial.println("Watchdog: restarting...");
    ESP.restart();
  }

  delay(5);
}
