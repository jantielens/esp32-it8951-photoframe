#include "rtc_mqtt_payload.h"

#if HAS_MQTT

#include <Arduino.h>

namespace {

static constexpr uint32_t kMagic = 0x4D515454; // 'MQTT'
static constexpr uint16_t kVersion = 1;

struct RtcMqttPayload {
  uint32_t magic;
  uint16_t version;
  uint16_t len;
  uint8_t data[MQTT_MAX_PACKET_SIZE];
};

RTC_DATA_ATTR RtcMqttPayload g_rtc_mqtt_payload;

static bool is_valid() {
  if (g_rtc_mqtt_payload.magic != kMagic) {
    return false;
  }
  if (g_rtc_mqtt_payload.version != kVersion) {
    return false;
  }
  if (g_rtc_mqtt_payload.len == 0) {
    return false;
  }
  if (g_rtc_mqtt_payload.len > sizeof(g_rtc_mqtt_payload.data)) {
    return false;
  }
  return true;
}

} // namespace

bool rtc_mqtt_payload_has() {
  return is_valid();
}

bool rtc_mqtt_payload_store(const uint8_t *data, size_t len) {
  if (!data || len == 0) {
    return false;
  }
  if (len > sizeof(g_rtc_mqtt_payload.data)) {
    return false;
  }

  g_rtc_mqtt_payload.magic = kMagic;
  g_rtc_mqtt_payload.version = kVersion;
  g_rtc_mqtt_payload.len = static_cast<uint16_t>(len);
  memcpy(g_rtc_mqtt_payload.data, data, len);
  return true;
}

bool rtc_mqtt_payload_take(uint8_t *out, size_t out_size, size_t *out_len) {
  if (!out || out_size == 0 || !out_len) {
    return false;
  }
  if (!is_valid()) {
    return false;
  }
  if (g_rtc_mqtt_payload.len > out_size) {
    return false;
  }

  memcpy(out, g_rtc_mqtt_payload.data, g_rtc_mqtt_payload.len);
  *out_len = g_rtc_mqtt_payload.len;

  rtc_mqtt_payload_clear();
  return true;
}

void rtc_mqtt_payload_clear() {
  g_rtc_mqtt_payload.magic = 0;
  g_rtc_mqtt_payload.version = 0;
  g_rtc_mqtt_payload.len = 0;
}

#endif // HAS_MQTT
