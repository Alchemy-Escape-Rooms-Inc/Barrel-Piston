// ============================================================
// MANIFEST.h — WatchTower Device Manifest
// This file is parsed by sync_manifests.py for the WatchTower dashboard.
// Keep all values as #define strings unless noted otherwise.
// ============================================================

#define DEVICE_NAME           "BarrelPiston"
#define FIRMWARE_VERSION      "2.1.0"
#define BOARD_TYPE            "ESP32-S3"
#define ROOM                  "MermaidsTale"
#define DESCRIPTION           "Pulse-based barrel piston controller with MQTT command/status/log topics, interlock protection, watchdog reboot, and device telemetry"

#define BUILD_STATUS          "stable"
#define CODE_HEALTH           "good"
#define WATCHTOWER_COMPLIANCE "full"

// MQTT
#define BROKER_IP             "10.1.10.115"
#define BROKER_PORT           1883
#define HEARTBEAT_MS          300000

#define SUBSCRIBE_TOPICS      "MermaidsTale/BarrelPiston/command"
#define PUBLISH_TOPICS        "MermaidsTale/BarrelPiston/status, MermaidsTale/BarrelPiston/log"
#define SUPPORTED_COMMANDS    "EXTEND, RETRACT, STOP, RESET, PUZZLE_RESET, STATUS, PING"

// Hardware
#define PIN_CONFIG            "RELAY_EXTEND=18, RELAY_RETRACT=8, LED=disabled"
#define COMPONENTS            "2x relay (pulse-based extend/retract with interlock), status LED"
#define KNOWN_QUIRKS          "Pulse timing must be tuned (PULSE_MS). Commands must not be retained. PubSubClient topic pointer can be unreliable, so topic is copied to a local buffer first in mqttCallback."

#define REPO_URL              "https://github.com/Alchemy-Escape-Rooms-Inc/Barrel-Piston"
