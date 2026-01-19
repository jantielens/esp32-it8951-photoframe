#include "rtc_state.h"

#include <Arduino.h>

namespace {
struct RtcImageState {
    uint32_t magic;
    uint32_t last_image_index;
};

static constexpr uint32_t kRtcImageStateMagic = 0x52544331; // "RTC1"
static constexpr uint32_t kRtcInvalidIndex = 0xFFFFFFFFu;

RTC_DATA_ATTR RtcImageState g_rtc_image_state;

static void rtc_image_state_reset() {
    g_rtc_image_state.magic = kRtcImageStateMagic;
    g_rtc_image_state.last_image_index = kRtcInvalidIndex;
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
