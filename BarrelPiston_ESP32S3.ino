#include <WiFi.h>
#include <PubSubClient.h>

// =====================================================
// IDENTITY / VERSION
// =====================================================
#define DEVICE_NAME "BarrelPiston"
#define FW_VERSION  "2.1.0"

// =====================================================
// CONFIG
// =====================================================
const char* ssid         = "AlchemyGuest";
const char* password     = "VoodooVacation5601";

const char* mqtt_host    = "10.1.10.115";
const uint16_t mqtt_port = 1883;
const char* mqtt_user    = "";
const char* mqtt_pass    = "";

// MQTT Topics (WatchTower standard: command, status, log)
const char* T_COMMAND    = "MermaidsTale/BarrelPiston/command";
const char* T_STATUS     = "MermaidsTale/BarrelPiston/status";
const char* T_LOG        = "MermaidsTale/BarrelPiston/log";

// Pins (ESP32-S3 compatible)
const int RELAY_EXTEND_PIN  = 18;
const int RELAY_RETRACT_PIN = 8;
const int LED_PIN           = -1;  // Disabled for ESP32-S3

// Timing
const uint32_t PULSE_MS             = 350;
const uint32_t INTERLOCK_GAP_MS     = 120;
const uint32_t HEARTBEAT_INTERVAL_MS = 300000;   // 5 minutes per WatchTower protocol
const uint32_t WATCHDOG_REBOOT_MS   = 600000;    // Reboot after 10 min inactivity
const uint32_t BACKOFF_MAX_MS       = 30000;

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
uint32_t pulse_start_ms = 0;
uint32_t interlock_start_ms = 0;
uint32_t last_activity_ms = 0;
uint32_t last_heartbeat_ms = 0;
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

void publishStatus(const char* payload) {
  if (mqttReady()) mqtt.publish(T_STATUS, payload, true);
}

void publishLog(const char* payload) {
  if (mqttReady()) mqtt.publish(T_LOG, payload, false);
}

void mqttLog(const char* msg) {
  Serial.println(msg);
  publishLog(msg);
}

void mqttLogf(const char* fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.println(buf);
  publishLog(buf);
}

bool tokenEquals(const char* a, const char* b) {
  return strcasecmp(a, b) == 0;
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

// =====================================================
// HEARTBEAT & STATUS (WatchTower protocol)
// =====================================================
void publishHeartbeat() {
  char buf[128];
  snprintf(buf, sizeof(buf), "HEARTBEAT:%s:UP%lus:RSSI%d",
    stateToString(state),
    millis() / 1000UL,
    WiFi.RSSI()
  );
  publishStatus(buf);
}

void publishDiagnostics() {
  char buf[256];
  snprintf(buf, sizeof(buf),
    "STATUS:%s|v%s|IP=%s|State=%s|Safety=%s|Pending=%s|Extend=%s|Retract=%s|Err=%s|UP%lus|RSSI%d",
    DEVICE_NAME,
    FW_VERSION,
    WiFi.localIP().toString().c_str(),
    stateToString(state),
    safety_latched ? "LATCHED" : "CLEAR",
    pendingToString(pendingAction),
    relay_extend_intended ? "ON" : "OFF",
    relay_retract_intended ? "ON" : "OFF",
    last_error,
    millis() / 1000UL,
    WiFi.RSSI()
  );
  publishStatus(buf);
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

  publishStatus(stateToString(state));
  mqttLogf("IDLE: %s", reason);
}

void enterSafety(const char* reason) {
  allRelaysOff();
  state = PistonState::SAFETY;
  pendingAction = PendingAction::NONE;
  safety_latched = true;

  setLastError(reason);
  publishStatus("SAFETY");
  led.set(80);

  mqttLogf("SAFETY: %s", reason);
}

void beginInterlock(PendingAction nextAction, const char* reason) {
  allRelaysOff();
  state = PistonState::INTERLOCK;
  pendingAction = nextAction;
  interlock_start_ms = millis();

  setLastError(reason);
  publishStatus("INTERLOCK");
  mqttLogf("INTERLOCK -> %s (%s)", pendingToString(nextAction), reason);
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
  publishStatus("PULSE_EXTEND");
  mqttLog("Pulse: EXTEND");
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
  publishStatus("PULSE_RETRACT");
  mqttLog("Pulse: RETRACT");
}

void processMotionState() {
  uint32_t now = millis();

  if (state == PistonState::PULSE_EXTEND || state == PistonState::PULSE_RETRACT) {
    if (now - pulse_start_ms >= PULSE_MS) {
      allRelaysOff();

      if (pendingAction == PendingAction::EXTEND || pendingAction == PendingAction::RETRACT) {
        beginInterlock(pendingAction, "QUEUED_DIRECTION_SWAP");
      } else {
        state = PistonState::IDLE;
        publishStatus("IDLE");
        mqttLog("Pulse complete -> IDLE");
      }
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
  mqttLogf("COMMAND: %s", msg);

  if (tokenEquals(msg, "PING")) {
    publishStatus("PONG");
    return;
  }

  if (tokenEquals(msg, "STATUS")) {
    publishDiagnostics();
    return;
  }

  if (tokenEquals(msg, "RESET")) {
    allRelaysOff();
    publishStatus("OK");
    mqttLog("RESET: rebooting...");
    delay(100);
    ESP.restart();
    return;
  }

  if (tokenEquals(msg, "PUZZLE_RESET")) {
    safety_latched = false;
    allRelaysOff();
    state = PistonState::IDLE;
    pendingAction = PendingAction::NONE;
    setLastError("NONE");
    publishStatus("OK");
    publishDiagnostics();
    led.solid(true);
    mqttLog("PUZZLE_RESET: game state cleared");
    return;
  }

  if (tokenEquals(msg, "STOP")) {
    allRelaysOff();
    state = PistonState::IDLE;
    pendingAction = PendingAction::NONE;
    publishStatus("IDLE");
    mqttLog("STOP -> IDLE");
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
  mqttLogf("ERROR: UNKNOWN_COMMAND '%s'", msg);
}

void handleCommand(const char* topic, const char* msg) {
  if (!strcmp(topic, T_COMMAND)) {
    handlePrimaryCommand(msg);
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

    publishStatus("ONLINE");
    publishDiagnostics();
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

  // Defensive: should never happen through normal code paths (setRelays is atomic)
  if (relay_extend_intended && relay_retract_intended) {
    enterSafety("HARDWARE_BOTH_RELAYS_ACTIVE");
  }

  processMotionState();

  uint32_t now = millis();

  if (mqtt.connected() && (now - last_heartbeat_ms >= HEARTBEAT_INTERVAL_MS)) {
    publishHeartbeat();
    last_heartbeat_ms = now;
  }

  if (last_activity_ms > 0 && (now - last_activity_ms > WATCHDOG_REBOOT_MS)) {
    mqttLog("Watchdog: restarting...");
    delay(100);
    ESP.restart();
  }

  delay(5);
}
