#include "rtc_state.h"

#include <Arduino.h>

namespace {
struct RtcImageState {
    uint32_t magic;
    uint32_t last_image_index;
    char last_image_name[RTC_IMAGE_NAME_MAX_LEN];
    char priority_image_name[RTC_IMAGE_NAME_MAX_LEN];
    char last_perm_name[RTC_IMAGE_NAME_MAX_LEN];
    char last_temp_name[RTC_IMAGE_NAME_MAX_LEN];
    bool last_was_temp;
};

struct RtcWifiState {
    uint32_t magic;
    uint32_t ssid_hash;
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi;
};

static constexpr uint32_t kRtcImageStateMagic = 0x52544332; // "RTC2"
static constexpr uint32_t kRtcWifiStateMagic = 0x52544357; // "RTCW"
static constexpr uint32_t kRtcInvalidIndex = 0xFFFFFFFFu;

RTC_DATA_ATTR RtcImageState g_rtc_image_state;
RTC_DATA_ATTR RtcWifiState g_rtc_wifi_state;

static void rtc_image_state_reset() {
    g_rtc_image_state.magic = kRtcImageStateMagic;
    g_rtc_image_state.last_image_index = kRtcInvalidIndex;
    g_rtc_image_state.last_image_name[0] = '\0';
    g_rtc_image_state.priority_image_name[0] = '\0';
    g_rtc_image_state.last_perm_name[0] = '\0';
    g_rtc_image_state.last_temp_name[0] = '\0';
    g_rtc_image_state.last_was_temp = false;
}

static uint32_t fnv1a_32(const char *s) {
    if (!s) return 0;
    uint32_t hash = 2166136261u;
    for (const uint8_t *p = reinterpret_cast<const uint8_t *>(s); *p; ++p) {
        hash ^= *p;
        hash *= 16777619u;
    }
    return hash;
}

static void rtc_wifi_state_reset() {
    g_rtc_wifi_state.magic = kRtcWifiStateMagic;
    g_rtc_wifi_state.ssid_hash = 0;
    memset(g_rtc_wifi_state.bssid, 0, sizeof(g_rtc_wifi_state.bssid));
    g_rtc_wifi_state.channel = 0;
    g_rtc_wifi_state.rssi = -127;
}
} // namespace

void rtc_image_state_init() {
    if (g_rtc_image_state.magic != kRtcImageStateMagic) {
        rtc_image_state_reset();
    }
}

void rtc_wifi_state_init() {
    if (g_rtc_wifi_state.magic != kRtcWifiStateMagic) {
        rtc_wifi_state_reset();
    }
}

bool rtc_wifi_state_get_best_ap(const char *ssid, uint8_t out_bssid[6], uint8_t *out_channel) {
    rtc_wifi_state_init();
    if (!ssid || strlen(ssid) == 0) return false;
    const uint32_t want = fnv1a_32(ssid);
    if (g_rtc_wifi_state.ssid_hash != want) return false;
    if (g_rtc_wifi_state.channel == 0) return false;
    if (out_bssid) memcpy(out_bssid, g_rtc_wifi_state.bssid, 6);
    if (out_channel) *out_channel = g_rtc_wifi_state.channel;
    return true;
}

void rtc_wifi_state_set_best_ap(const char *ssid, const uint8_t bssid[6], uint8_t channel, int8_t rssi) {
    rtc_wifi_state_init();
    if (!ssid || strlen(ssid) == 0) return;
    if (!bssid || channel == 0) return;
    g_rtc_wifi_state.ssid_hash = fnv1a_32(ssid);
    memcpy(g_rtc_wifi_state.bssid, bssid, 6);
    g_rtc_wifi_state.channel = channel;
    g_rtc_wifi_state.rssi = rssi;
}

void rtc_wifi_state_clear() {
    rtc_wifi_state_reset();
}

uint32_t rtc_image_state_get_last_image_index() {
    rtc_image_state_init();
    return g_rtc_image_state.last_image_index;
}

void rtc_image_state_set_last_image_index(uint32_t index) {
    rtc_image_state_init();
    g_rtc_image_state.last_image_index = index;
}

const char* rtc_image_state_get_last_image_name() {
    rtc_image_state_init();
    return g_rtc_image_state.last_image_name;
}

void rtc_image_state_set_last_image_name(const char *name) {
    rtc_image_state_init();
    if (!name) {
        g_rtc_image_state.last_image_name[0] = '\0';
        return;
    }
    strlcpy(g_rtc_image_state.last_image_name, name, sizeof(g_rtc_image_state.last_image_name));
}

const char* rtc_image_state_get_last_perm_name() {
    rtc_image_state_init();
    return g_rtc_image_state.last_perm_name;
}

void rtc_image_state_set_last_perm_name(const char *name) {
    rtc_image_state_init();
    if (!name) {
        g_rtc_image_state.last_perm_name[0] = '\0';
        return;
    }
    strlcpy(g_rtc_image_state.last_perm_name, name, sizeof(g_rtc_image_state.last_perm_name));
}

const char* rtc_image_state_get_last_temp_name() {
    rtc_image_state_init();
    return g_rtc_image_state.last_temp_name;
}

void rtc_image_state_set_last_temp_name(const char *name) {
    rtc_image_state_init();
    if (!name) {
        g_rtc_image_state.last_temp_name[0] = '\0';
        return;
    }
    strlcpy(g_rtc_image_state.last_temp_name, name, sizeof(g_rtc_image_state.last_temp_name));
}

bool rtc_image_state_get_last_was_temp() {
    rtc_image_state_init();
    return g_rtc_image_state.last_was_temp;
}

void rtc_image_state_set_last_was_temp(bool was_temp) {
    rtc_image_state_init();
    g_rtc_image_state.last_was_temp = was_temp;
}

const char* rtc_image_state_get_priority_image_name() {
    rtc_image_state_init();
    return g_rtc_image_state.priority_image_name;
}

void rtc_image_state_set_priority_image_name(const char *name) {
    rtc_image_state_init();
    if (!name) {
        g_rtc_image_state.priority_image_name[0] = '\0';
        return;
    }
    strlcpy(g_rtc_image_state.priority_image_name, name, sizeof(g_rtc_image_state.priority_image_name));
}

void rtc_image_state_clear_priority_image_name() {
    rtc_image_state_init();
    g_rtc_image_state.priority_image_name[0] = '\0';
}
