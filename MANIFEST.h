// ============================================================
// MANIFEST.h — WatchTower Device Manifest
// This file is parsed by sync_manifests.py for the WatchTower dashboard.
// Keep all values as #define strings unless noted otherwise.
// ============================================================

#define DEVICE_NAME           "BarrelPiston"
#define FIRMWARE_VERSION      "2.0.1"
#define BOARD_TYPE            "ESP32-S3"
#define ROOM                  "MermaidsTale"
#define DESCRIPTION           "Pulse-based barrel piston controller with single MQTT command topic, interlock protection, GPIO2/4 aux outputs, watchdog reboot, and device telemetry"

#define BUILD_STATUS          "stable"
#define CODE_HEALTH           "good"
#define WATCHTOWER_COMPLIANCE "full"

// MQTT
#define BROKER_IP             "10.1.10.115"
#define BROKER_PORT           1883
#define HEARTBEAT_MS          30000

#define SUBSCRIBE_TOPICS      "MermaidsTale/BarrelPiston/command, MermaidsTale/BarrelPiston/GPIO2, MermaidsTale/BarrelPiston/GPIO4"
#define PUBLISH_TOPICS        "MermaidsTale/BarrelPiston/status, MermaidsTale/BarrelPiston/state, MermaidsTale/BarrelPiston/safety, MermaidsTale/BarrelPiston/info, MermaidsTale/BarrelPiston/version, MermaidsTale/BarrelPiston/device, MermaidsTale/BarrelPiston/lastError, MermaidsTale/BarrelPiston/uptime, MermaidsTale/BarrelPiston/GPIO2/State, MermaidsTale/BarrelPiston/GPIO4/State"
#define SUPPORTED_COMMANDS    "EXTEND, RETRACT, STOP, RESET, STATUS, PING, GPIO2(HIGH/LOW/READ), GPIO4(HIGH/LOW/READ)"

// Hardware
#define PIN_CONFIG            "RELAY_EXTEND=18, RELAY_RETRACT=8, LED=disabled, AUX_GPIO2=2, AUX_GPIO4=4"
#define COMPONENTS            "2x relay (pulse-based extend/retract with interlock), status LED, 2x aux GPIO"
#define KNOWN_QUIRKS          "Pulse timing must be tuned (PULSE_MS). Commands must not be retained. PubSubClient topic pointer can be unreliable, so topic is copied to a local buffer first in mqttCallback."

#define REPO_URL              "https://github.com/Alchemy-Escape-Rooms-Inc/Barrel-Piston"
