
#include <WiFi.h>
#include <PubSubClient.h>

// =====================================================
// IDENTITY / VERSION
// =====================================================
#define DEVICE_NAME "BarrelPiston"
#define FW_VERSION  "BarrelPiston v2.0.0 (Pulse Mode)"

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

// Pins
const int RELAY_EXTEND_PIN  = 16;
const int RELAY_RETRACT_PIN = 17;
const int LED_PIN           = 25;

// Timing
const uint32_t PULSE_MS           = 350;
const uint32_t INTERLOCK_GAP_MS   = 120;
const uint32_t STATUS_INTERVAL_MS = 30000;

// Globals
WiFiClient wifi;
PubSubClient mqtt(wifi);

enum class PistonState : uint8_t {
  IDLE,
  PULSE_EXTEND,
  PULSE_RETRACT,
  INTERLOCK,
  SAFETY
};

PistonState state = PistonState::IDLE;
bool safety_latched = false;

uint32_t pulse_start_ms = 0;
uint32_t interlock_start_ms = 0;
uint32_t last_status_publish_ms = 0;

char last_error[96] = "NONE";

// =====================================================
// HELPERS
// =====================================================
void publishRetained(const char* topic, const char* payload) {
  if (mqtt.connected()) mqtt.publish(topic, payload, true);
}

bool tokenEquals(const char* a, const char* b) {
  return strcasecmp(a, b) == 0;
}

const char* stateToString(PistonState s) {
  switch (s) {
    case PistonState::IDLE: return "IDLE";
    case PistonState::PULSE_EXTEND: return "PULSE_EXTEND";
    case PistonState::PULSE_RETRACT: return "PULSE_RETRACT";
    case PistonState::INTERLOCK: return "INTERLOCK";
    case PistonState::SAFETY: return "SAFETY";
    default: return "UNKNOWN";
  }
}

void setRelays(bool extend_on, bool retract_on) {
  digitalWrite(RELAY_EXTEND_PIN,  extend_on  ? HIGH : LOW);
  digitalWrite(RELAY_RETRACT_PIN, retract_on ? HIGH : LOW);
}

void allRelaysOff() {
  setRelays(false, false);
}

void publishStatus() {
  char uptime[32];
  snprintf(uptime, sizeof(uptime), "%lu", millis()/1000UL);

  publishRetained(T_DEVICE, DEVICE_NAME);
  publishRetained(T_VERSION, FW_VERSION);
  publishRetained(T_STATUS, "ONLINE");
  publishRetained(T_STATE, stateToString(state));
  publishRetained(T_SAFETY, safety_latched ? "LATCHED" : "CLEAR");
  publishRetained(T_LASTERROR, last_error);
  publishRetained(T_UPTIME, uptime);
}

// =====================================================
// STATE CONTROL
// =====================================================
void startPulseExtend() {
  allRelaysOff();
  setRelays(true, false);
  state = PistonState::PULSE_EXTEND;
  pulse_start_ms = millis();
}

void startPulseRetract() {
  allRelaysOff();
  setRelays(false, true);
  state = PistonState::PULSE_RETRACT;
  pulse_start_ms = millis();
}

void processMotion() {
  uint32_t now = millis();

  if (state == PistonState::PULSE_EXTEND || state == PistonState::PULSE_RETRACT) {
    if (now - pulse_start_ms >= PULSE_MS) {
      allRelaysOff();
      state = PistonState::IDLE;
    }
  }
}

// =====================================================
// COMMANDS
// =====================================================
void handleCommand(const char* msg) {
  if (tokenEquals(msg, "PING")) {
    mqtt.publish(T_INFO, "PONG");
    return;
  }

  if (tokenEquals(msg, "STATUS")) {
    publishStatus();
    return;
  }

  if (tokenEquals(msg, "RESET")) {
    safety_latched = false;
    state = PistonState::IDLE;
    strcpy(last_error, "NONE");
    publishStatus();
    return;
  }

  if (tokenEquals(msg, "STOP")) {
    allRelaysOff();
    state = PistonState::IDLE;
    return;
  }

  if (tokenEquals(msg, "ENGAGE") || tokenEquals(msg, "EXTEND")) {
    startPulseExtend();
    return;
  }

  if (tokenEquals(msg, "RETRACT")) {
    startPulseRetract();
    return;
  }
}

// =====================================================
// MQTT
// =====================================================
void callback(char* topic, byte* payload, unsigned int length) {
  char msg[128];
  memcpy(msg, payload, length);
  msg[length] = '\0';

  handleCommand(msg);
}

void reconnect() {
  if (mqtt.connected()) return;

  if (mqtt.connect("BarrelPistonClient")) {
    mqtt.subscribe(T_COMMAND);
    publishStatus();
  }
}

// =====================================================
// SETUP / LOOP
// =====================================================
void setup() {
  Serial.begin(115200);

  pinMode(RELAY_EXTEND_PIN, OUTPUT);
  pinMode(RELAY_RETRACT_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);

  allRelaysOff();

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  mqtt.setServer(mqtt_host, mqtt_port);
  mqtt.setCallback(callback);
}

void loop() {
  if (!mqtt.connected()) {
    reconnect();
  }
  mqtt.loop();

  processMotion();

  uint32_t now = millis();
  if (now - last_status_publish_ms > STATUS_INTERVAL_MS) {
    publishStatus();
    last_status_publish_ms = now;
  }
}
