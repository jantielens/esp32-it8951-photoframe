#pragma once

#include <stddef.h>
#include <stdint.h>

#include "board_config.h"

// RTC-retained MQTT payload buffer (deep sleep persistence, not NVS).
//
// Intended use:
// - Capture a JSON state payload near the end of a cycle.
// - On next wake, publish the stored payload once and clear it.
//
// Best-effort semantics:
// - Data may be lost on power loss/brownout.
// - If the stored payload is invalid, it is ignored.

#if HAS_MQTT

// Keep this consistent with mqtt_manager.h so the deferred buffer can always
// hold any payload that PubSubClient is configured to publish.
#ifndef MQTT_MAX_PACKET_SIZE
#define MQTT_MAX_PACKET_SIZE 1024
#endif

bool rtc_mqtt_payload_has();

// Store a payload for the next boot. Returns false if too large.
bool rtc_mqtt_payload_store(const uint8_t *data, size_t len);

// Copy out the stored payload and clear it.
// Returns false if no valid payload is stored.
bool rtc_mqtt_payload_take(uint8_t *out, size_t out_size, size_t *out_len);

// Clear any stored payload.
void rtc_mqtt_payload_clear();

#endif // HAS_MQTT
