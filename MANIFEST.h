// ============================================================
// MANIFEST.h — WatchTower Device Manifest
// This file is parsed by sync_manifests.py for the WatchTower dashboard.
// Keep all values as #define strings unless noted otherwise.
// ============================================================

#define DEVICE_NAME           "BarrelPiston"
#define FIRMWARE_VERSION      "3.2.3"
#define BOARD_TYPE            "ESP32-S3"
#define ROOM                  "MermaidsTale"
#define DESCRIPTION           "Continuous-mode barrel piston controller: relay stays energized until STOP/opposite command, 2-min auto-stop safety, interlock gap on direction change, GPIO2/4 aux outputs, connectivity watchdog, device telemetry, ArduinoOTA wireless re-flash"

#define BUILD_STATUS          "stable"
#define CODE_HEALTH           "good"
#define WATCHTOWER_COMPLIANCE "full"

// MQTT
#define BROKER_IP             "10.1.10.115"
#define BROKER_PORT           1883
#define HEARTBEAT_MS          300000

// Over-the-air updates
#define OTA_ENABLED           "yes"
#define OTA_HOSTNAME          "BarrelPiston"
#define OTA_PORT              3232

#define SUBSCRIBE_TOPICS      "MermaidsTale/BarrelPiston/command, MermaidsTale/BarrelPiston/GPIO2, MermaidsTale/BarrelPiston/GPIO4"
#define PUBLISH_TOPICS        "MermaidsTale/BarrelPiston/status, MermaidsTale/BarrelPiston/log, MermaidsTale/BarrelPiston/state, MermaidsTale/BarrelPiston/safety, MermaidsTale/BarrelPiston/info, MermaidsTale/BarrelPiston/version, MermaidsTale/BarrelPiston/device, MermaidsTale/BarrelPiston/lastError, MermaidsTale/BarrelPiston/uptime, MermaidsTale/BarrelPiston/GPIO2/State, MermaidsTale/BarrelPiston/GPIO4/State"
#define SUPPORTED_COMMANDS    "EXTEND, RETRACT, STOP, LOGIC_HIGH, LOGIC_LOW, RESET, PUZZLE_RESET, STATUS, PING, GPIO2(HIGH/LOW/READ), GPIO4(HIGH/LOW/READ)"

// Hardware
#define PIN_CONFIG            "RELAY_EXTEND=18, RELAY_RETRACT=8 (relay boards are HIGH-level trigger: ON = pin HIGH, OFF = pin LOW; switchable with LOGIC_HIGH/LOGIC_LOW), LED=disabled, AUX_GPIO2=2, AUX_GPIO4=4"
#define COMPONENTS            "2x relay (pulse-based extend/retract with interlock), status LED, 2x aux GPIO"
#define KNOWN_QUIRKS          "Commands must not be retained. PubSubClient topic pointer can be unreliable, so topic is copied to a local buffer first in mqttCallback. Watchdog reboots only after 10 min with no broker connectivity (a connected idle session counts as activity). Device PONGs/OKs on /command and ignores its own replies. M3 AMT.xml sends RESET on game reset, which now reboots the board per protocol - consider switching AMT.xml to PUZZLE_RESET."

#define REPO_URL              "https://github.com/Alchemy-Escape-Rooms-Inc/Barrel-Piston"
