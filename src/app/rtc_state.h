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

// Separate queues for queue-permanent/queue-temporary rotation (RTC retained across deep sleep).
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

// RTC retained state for WiFi AP hinting (deep sleep persistence).
// Stores the last known best AP for a given SSID (BSSID + channel).
void rtc_wifi_state_init();
bool rtc_wifi_state_get_best_ap(const char *ssid, uint8_t out_bssid[6], uint8_t *out_channel);
void rtc_wifi_state_set_best_ap(const char *ssid, const uint8_t bssid[6], uint8_t channel, int8_t rssi);
void rtc_wifi_state_clear();
