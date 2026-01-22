# Home Assistant + MQTT Integration

This project includes an optional MQTT integration designed for Home Assistant (HA) using **MQTT Discovery**.

Key design choices:
- **Single JSON state topic** (batched payload) for efficiency
- **HA MQTT Discovery** for auto-creating entities
- **Retained** discovery + state so HA can restart without losing entities/values
- System/health entities are marked as **Diagnostic** in HA (`entity_category: diagnostic`)

## Prerequisites

- A reachable MQTT broker (Mosquitto, HA add-on, etc.)
- Home Assistant with the MQTT integration enabled
- Firmware built with `HAS_MQTT` enabled

## Enable/Disable MQTT in Firmware

MQTT support is controlled at compile time:
- Default is defined in `src/app/board_config.h`
- You can override per-board via `src/boards/<board>/board_overrides.h`

When `HAS_MQTT` is disabled:
- The UI hides MQTT settings (capability flag)
- MQTT code is compiled out

## Configure MQTT (Web Portal)

Go to the **Network** page and fill in:
- `MQTT Host` (required to enable MQTT)
- `MQTT Port` (optional, default 1883)
- `MQTT Username` / `MQTT Password` (optional)
- `Publish Interval (seconds)`

Behavior:
- If `MQTT Host` is empty: device will not connect.
- If `MQTT Host` is set:
  - Device connects and publishes discovery (see note below).
  - Device publishes **one retained** state payload right after connect (see notes below).
  - If `Publish Interval > 0`: it also republishes state periodically.

Discovery note:
- Discovery is published **only on non-deep-sleep resets** (power-on, software reset, etc.).
- In SleepCycle mode (deep sleep wakes), discovery is skipped to reduce MQTT traffic; the retained discovery config published earlier remains valid in Home Assistant.

Sleep-cycle note:
- In **SleepCycle** mode, the firmware captures the state payload at the end of the cycle (right before deep sleep) and publishes it on the *next* wake.
- On the very first boot (or if RTC data was lost), the firmware publishes a one-time **boot snapshot** so HA shows values immediately.
- After that, HA will typically show **previous-cycle** values (no timestamps).

## Topics

The device derives a **sanitized name** from the configured device name and uses it to build topics.

- Base topic: `devices/<sanitized>`
- State (JSON): `devices/<sanitized>/health/state` (retained JSON)

Home Assistant staleness handling:
- Entities are configured with `expire_after` via discovery.
- `expire_after` is derived from the device config (sleep timeout / publish interval + margin), so HA keeps showing the last values during deep sleep and only marks entities `unavailable` if the device misses an expected wake/publish.

Note:
- The firmware may still publish an MQTT availability/LWT topic (`devices/<sanitized>/availability`), but HA entities are not configured to use it (sleep would otherwise mark entities unavailable immediately).

Home Assistant discovery topics:
- `homeassistant/sensor/<sanitized>/<object_id>/config` (retained)
- `homeassistant/binary_sensor/<sanitized>/<object_id>/config` (retained)

## State Payload (JSON)

The state topic publishes one JSON document that contains multiple fields (examples):
- `uptime_seconds`
- `cycle_awake_seconds`
- `reset_reason`
- `cpu_usage`
- `cpu_temperature`
- `heap_free`, `heap_min`, `heap_largest`
- `heap_internal_free`, `heap_internal_min`, `heap_internal_largest`
- `psram_free`, `psram_min`, `psram_largest`
- `flash_used`, `flash_total`
- `fs_mounted`, `fs_used_bytes`, `fs_total_bytes`
- `wifi_rssi`

Note:
- The web API `/api/health` includes additional `mqtt_*` self-report fields for debugging.
- The MQTT state payload intentionally omits those `mqtt_*` fields.

Additional note:
- Fragmentation and display performance counters are exposed via the web API (`/api/health`) but are intentionally not included in the MQTT payload.

With `expire_after` enabled, the staleness/availability in HA is based on the last received state update.

Home Assistant entities use `value_template` to extract a single field, e.g.

- `{{ value_json.cpu_usage }}`

## Adding Custom Sensors (Step-by-Step)

This project is intentionally lightweight: add a JSON key + add a discovery entry.

### 1) Add your JSON fields to the MQTT payload

Edit `src/app/device_telemetry.cpp` in `device_telemetry_fill_mqtt()`.

Example (ambient sensor):

```cpp
// Example: external temperature/humidity
// doc["temperature"] = 23.4;
// doc["humidity"] = 55.2;
```

Notes:
- `cpu_temperature` is reserved for SoC/internal temperature.
- `temperature` is intended for external/ambient temperature.

### 2) Register Home Assistant entities via discovery

Edit `src/app/ha_discovery.cpp` in `ha_discovery_publish_health()` (kept at the top of the file).

Example (normal Sensors category):

```cpp
// publish_sensor_config(mqtt, "temperature", "Temperature", "{{ value_json.temperature }}", "°C", "temperature", "measurement", nullptr);
// publish_sensor_config(mqtt, "humidity", "Humidity", "{{ value_json.humidity }}", "%", "humidity", "measurement", nullptr);
```

Tip:
- Pass `"diagnostic"` as the last argument to put an entity in HA’s Diagnostic category.
- Pass `nullptr` (or omit the argument if you add your own wrapper) to keep it as a normal Sensor.

### 3) Build and flash

```bash
./build.sh esp32c3-waveshare-169-st7789v2
./upload.sh esp32c3-waveshare-169-st7789v2
./monitor.sh
```

### 4) If entities don’t update in Home Assistant

Discovery and state are **retained**, so HA may keep old configs if you change them.

Typical reset options:
- Remove the device/entities in HA and reboot the device.
- Or delete retained discovery topics under `homeassistant/sensor/<sanitized>/.../config` and reboot.

## Event-Driven Sensors (Presence)

Sometimes you want **most telemetry** to keep publishing on an interval, but a specific sensor (like **presence**) to update **immediately when it changes**.

There are two viable approaches.

### Option A (Quick + Simple): Force-publish the existing JSON state immediately

This keeps the “single JSON state topic” design intact:
- Leave your existing interval publishing enabled (`Publish Interval (seconds) > 0`).
- When presence changes, publish an immediate update by republishing the same retained JSON state topic.

Tradeoff:
- Presence will still be present in the JSON payload, so it will also get re-sent during interval publishes (even if unchanged).

**Recommended when:** you don’t care about presence being included in periodic publishes.

**Example (called from your sensor code on edge):**

```cpp
#include <ArduinoJson.h>
#include "device_telemetry.h"

// mqtt is your MqttManager instance
void publish_presence_edge_force_health_state(MqttManager &mqtt) {
    if (!mqtt.connected()) return;

    JsonDocument doc;
    device_telemetry_fill_mqtt(doc);
    mqtt.publishJson(mqtt.healthStateTopic(), doc, true);
}
```

Where to store the presence value:
- Keep the latest presence state in a global/module-level variable (or getter) that `device_telemetry_fill_mqtt()` reads when it builds the payload.

### Option B (Recommended): Separate presence topic (event-driven) + keep interval telemetry

This keeps interval telemetry clean and makes presence fully event-driven:
- Keep the existing interval JSON telemetry on: `devices/<sanitized>/health/state`
- Add a dedicated presence topic:
  - `devices/<sanitized>/presence/state` with payload `ON` / `OFF` (retained)
- Register presence in HA as a **binary_sensor** that points at that topic

Benefits:
- Presence updates only when it changes (no periodic republish).
- HA gets the right entity type (binary sensor) and semantics.

**Suggested additions (minimal API surface):**

1) Add a helper publish function (example) somewhere in your app code:

```cpp
// mqtt is your MqttManager instance
// presence is your boolean state
void publish_presence_state(MqttManager &mqtt, bool presence) {
    if (!mqtt.connected()) return;

    char topic[160];
    snprintf(topic, sizeof(topic), "%s/presence/state", mqtt.baseTopic());

    mqtt.publishImmediate(topic, presence ? "ON" : "OFF", true);
}
```

2) Add HA discovery for the presence binary_sensor.

Today, [src/app/ha_discovery.cpp](src/app/ha_discovery.cpp) publishes both `sensor` and `binary_sensor` discovery entries for the default health payload.
For presence, the recommended pattern is publishing a **separate presence state topic**, then registering a presence `binary_sensor` that points at that topic:
- topic: `homeassistant/binary_sensor/<sanitized>/<object_id>/config`
- `stat_t`: `~/presence/state`
- `pl_on`: `ON`
- `pl_off`: `OFF`

Example config payload shape (pseudo-code):

```cpp
// publish_binary_sensor_config(mqtt,
//     "presence",
//     "Presence",
//     "presence",   // device_class
//     nullptr        // entity_category
// );
```

Operational notes:
- Use retained publishes for presence so HA can recover the last known state after restart.
- Add basic debouncing in your sensor logic so you don’t spam MQTT during noisy transitions.

## Troubleshooting

- Nothing appears in HA:
  - Verify MQTT broker address/credentials.
  - Check serial logs for `[MQTT] Connected` and `Publishing HA discovery`.
- Entities exist but values are `unknown`:
  - Ensure the state topic is being published (`devices/<sanitized>/health/state`).
  - Verify the JSON key matches the `value_template`.
- You changed names and got duplicate entities:
  - Clear retained discovery topics and reboot.
