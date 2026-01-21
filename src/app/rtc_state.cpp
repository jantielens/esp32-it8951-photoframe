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

static constexpr uint32_t kRtcImageStateMagic = 0x52544332; // "RTC2"
static constexpr uint32_t kRtcInvalidIndex = 0xFFFFFFFFu;

RTC_DATA_ATTR RtcImageState g_rtc_image_state;

static void rtc_image_state_reset() {
    g_rtc_image_state.magic = kRtcImageStateMagic;
    g_rtc_image_state.last_image_index = kRtcInvalidIndex;
    g_rtc_image_state.last_image_name[0] = '\0';
    g_rtc_image_state.priority_image_name[0] = '\0';
    g_rtc_image_state.last_perm_name[0] = '\0';
    g_rtc_image_state.last_temp_name[0] = '\0';
    g_rtc_image_state.last_was_temp = false;
}
} // namespace

void rtc_image_state_init() {
    if (g_rtc_image_state.magic != kRtcImageStateMagic) {
        rtc_image_state_reset();
    }
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
