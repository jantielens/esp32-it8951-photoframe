#pragma once

#include <stdint.h>

// Max stored filename length (including null terminator).
#define RTC_IMAGE_NAME_MAX_LEN 64

// RTC retained state for sequential image index (deep sleep persistence).
void rtc_image_state_init();
uint32_t rtc_image_state_get_last_image_index();
void rtc_image_state_set_last_image_index(uint32_t index);
const char* rtc_image_state_get_last_image_name();
void rtc_image_state_set_last_image_name(const char *name);
