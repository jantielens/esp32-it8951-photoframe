#pragma once

#include <stdint.h>

// Max stored filename length (including null terminator).
#define RTC_IMAGE_NAME_MAX_LEN 128

// RTC retained state for sequential image index (deep sleep persistence).
void rtc_image_state_init();
uint32_t rtc_image_state_get_last_image_index();
void rtc_image_state_set_last_image_index(uint32_t index);
const char* rtc_image_state_get_last_image_name();
void rtc_image_state_set_last_image_name(const char *name);

// Separate queues for perm/temp rotation (RTC retained across deep sleep).
const char* rtc_image_state_get_last_perm_name();
void rtc_image_state_set_last_perm_name(const char *name);
const char* rtc_image_state_get_last_temp_name();
void rtc_image_state_set_last_temp_name(const char *name);
// Used to alternate between perm and temp when both are available.
bool rtc_image_state_get_last_was_temp();
void rtc_image_state_set_last_was_temp(bool was_temp);

// Priority image (render once, then clear).
const char* rtc_image_state_get_priority_image_name();
void rtc_image_state_set_priority_image_name(const char *name);
void rtc_image_state_clear_priority_image_name();
