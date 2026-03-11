// ============================================================
// MANIFEST.h — WatchTower Device Manifest
// This file is parsed by sync_manifests.py for the WatchTower dashboard.
// Keep all values as #define strings unless noted otherwise.
// ============================================================

#define DEVICE_NAME           "BarrelPiston"
#define FIRMWARE_VERSION      "1.0.0"
#define BOARD_TYPE            "ESP32"
#define ROOM                  "MermaidsTale"
#define DESCRIPTION           "Dual-relay barrel piston controller with safety interlocks, GPIO2/4 aux outputs, and watchdog reboot"

#define BUILD_STATUS          "stable"
#define CODE_HEALTH           "good"
#define WATCHTOWER_COMPLIANCE "full"

// MQTT
#define BROKER_IP             "10.1.10.115"
#define BROKER_PORT           1883
#define HEARTBEAT_MS          0

#define SUBSCRIBE_TOPICS      "MermaidsTale/BarrelPiston/Engaged, MermaidsTale/BarrelPiston/Retract, MermaidsTale/BarrelPiston/GPIO2, MermaidsTale/BarrelPiston/GPIO4"
#define PUBLISH_TOPICS        "MermaidsTale/BarrelPiston/Status, MermaidsTale/BarrelPiston/Safety, MermaidsTale/BarrelPiston/DeviceInfo, MermaidsTale/BarrelPiston/GPIO2/State, MermaidsTale/BarrelPiston/GPIO4/State"
#define SUPPORTED_COMMANDS    "Engaged(1/0), Retract(1/0), GPIO2(HIGH/LOW/READ), GPIO4(HIGH/LOW/READ)"

// Hardware
#define PIN_CONFIG            "RELAY_ENGAGE=16, RELAY_RETRACT=17, LED=25, AUX_GPIO2=2, AUX_GPIO4=4"
#define COMPONENTS            "2x relay (engage/retract with interlock), status LED, 2x aux GPIO"
#define KNOWN_QUIRKS          "PubSubClient topic pointer stack corruption — topic must be copied to local buffer first in mqttCallback"

#define REPO_URL              "https://github.com/Alchemy-Escape-Rooms-Inc/Barrel-Piston"
