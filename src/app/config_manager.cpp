/*
 * Configuration Manager Implementation
 * 
 * Uses ESP32 Preferences library (NVS wrapper) for persistent storage.
 * Stores configuration in "device_cfg" namespace.
 */

#include "config_manager.h"
#include "board_config.h"
#include "web_assets.h"
#include "log_manager.h"
#include <Preferences.h>
#include <nvs_flash.h>

// NVS namespace
#define CONFIG_NAMESPACE "device_cfg"

// Preferences keys
#define KEY_WIFI_SSID      "wifi_ssid"
#define KEY_WIFI_PASS      "wifi_pass"
#define KEY_DEVICE_NAME    "device_name"
#define KEY_FIXED_IP       "fixed_ip"
#define KEY_SUBNET_MASK    "subnet_mask"
#define KEY_GATEWAY        "gateway"
#define KEY_DNS1           "dns1"
#define KEY_DNS2           "dns2"
#define KEY_DUMMY          "dummy"
#define KEY_SLEEP_TIMEOUT  "sleep_to"
#define KEY_IMAGE_SELECT   "img_sel"
#define KEY_LONG_PRESS     "lp_ms"
#define KEY_ALWAYS_ON      "always_on"
#define KEY_MQTT_HOST      "mqtt_host"
#define KEY_MQTT_PORT      "mqtt_port"
#define KEY_MQTT_USER      "mqtt_user"
#define KEY_MQTT_PASS      "mqtt_pass"
#define KEY_MQTT_INTERVAL  "mqtt_int"
#define KEY_BACKLIGHT_BRIGHTNESS "bl_bright"

// Azure Blob pull-on-wake
#define KEY_BLOB_SAS_URL   "blob_sas"

// Web portal Basic Auth
#define KEY_BASIC_AUTH_ENABLED "ba_en"
#define KEY_BASIC_AUTH_USER    "ba_user"
#define KEY_BASIC_AUTH_PASS    "ba_pass"
#define KEY_MAGIC          "magic"

static Preferences preferences;

// Initialize NVS
void config_manager_init() {
    LOGI("Config", "NVS init start");
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        LOGW("Config", "NVS init error (%d) - erasing NVS", (int)err);
        nvs_flash_erase();
        err = nvs_flash_init();
    }

    if (err != ESP_OK) {
        LOGE("Config", "NVS init FAILED (%d)", (int)err);
        return;
    }

    LOGI("Config", "NVS init OK");
}

// Get default device name with unique chip ID
String config_manager_get_default_device_name() {
    uint32_t chipId = 0;
    for (int i = 0; i < 17; i = i + 8) {
        chipId |= ((ESP.getEfuseMac() >> (40 - i)) & 0xff) << i;
    }
    char name[32];
    snprintf(name, sizeof(name), PROJECT_DISPLAY_NAME " %04X", (uint16_t)(chipId & 0xFFFF));
    return String(name);
}

// Sanitize device name for mDNS (lowercase, alphanumeric + hyphens only)
void config_manager_sanitize_device_name(const char *input, char *output, size_t max_len) {
    if (!input || !output || max_len == 0) return;
    
    size_t j = 0;
    for (size_t i = 0; input[i] != '\0' && j < max_len - 1; i++) {
        char c = input[i];
        
        // Convert to lowercase
        if (c >= 'A' && c <= 'Z') {
            c = c + ('a' - 'A');
        }
        
        // Keep alphanumeric and convert spaces/special chars to hyphens
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            output[j++] = c;
        } else if (c == ' ' || c == '_' || c == '-') {
            // Don't add hyphen if previous char was already a hyphen
            if (j > 0 && output[j-1] != '-') {
                output[j++] = '-';
            }
        }
    }
    
    // Remove trailing hyphen if present
    if (j > 0 && output[j-1] == '-') {
        j--;
    }
    
    output[j] = '\0';
}

// Load configuration from NVS
bool config_manager_load(DeviceConfig *config) {
    if (!config) {
        LOGE("Config", "Load failed: NULL pointer");
        return false;
    }

    LOGI("Config", "Load start");

    if (!preferences.begin(CONFIG_NAMESPACE, true)) { // Read-only mode
        LOGE("Config", "Preferences begin failed");
        return false;
    }
    
    // Check magic number first
    uint32_t magic = preferences.getUInt(KEY_MAGIC, 0);
    if (magic != CONFIG_MAGIC) {
        preferences.end();
        LOGW("Config", "No config found");
        
        // Initialize defaults for fields that need sensible values even when no config exists
        config->backlight_brightness = 100;  // Default to full brightness
        config->sleep_timeout_seconds = 60;
        strlcpy(config->image_selection_mode, "random", CONFIG_IMAGE_SELECTION_MODE_MAX_LEN);
        config->long_press_ms = 1500;
        config->always_on = false;
        config->mqtt_port = 0;
        config->mqtt_interval_seconds = 0;

        // Azure Blob defaults
        config->blob_sas_url[0] = '\0';

        // Basic Auth defaults
        config->basic_auth_enabled = false;
        config->basic_auth_username[0] = '\0';
        config->basic_auth_password[0] = '\0';

        return false;
    }
    
    // Load WiFi settings
    preferences.getString(KEY_WIFI_SSID, config->wifi_ssid, CONFIG_SSID_MAX_LEN);
    preferences.getString(KEY_WIFI_PASS, config->wifi_password, CONFIG_PASSWORD_MAX_LEN);
    
    // Load device settings
    String default_name = config_manager_get_default_device_name();
    preferences.getString(KEY_DEVICE_NAME, config->device_name, CONFIG_DEVICE_NAME_MAX_LEN);
    if (strlen(config->device_name) == 0) {
        strlcpy(config->device_name, default_name.c_str(), CONFIG_DEVICE_NAME_MAX_LEN);
    }
    
    // Load fixed IP settings
    preferences.getString(KEY_FIXED_IP, config->fixed_ip, CONFIG_IP_STR_MAX_LEN);
    preferences.getString(KEY_SUBNET_MASK, config->subnet_mask, CONFIG_IP_STR_MAX_LEN);
    preferences.getString(KEY_GATEWAY, config->gateway, CONFIG_IP_STR_MAX_LEN);
    preferences.getString(KEY_DNS1, config->dns1, CONFIG_IP_STR_MAX_LEN);
    preferences.getString(KEY_DNS2, config->dns2, CONFIG_IP_STR_MAX_LEN);
    
    // Load dummy setting
    preferences.getString(KEY_DUMMY, config->dummy_setting, CONFIG_DUMMY_MAX_LEN);

    // Load phase 2 settings
    config->sleep_timeout_seconds = preferences.getUShort(KEY_SLEEP_TIMEOUT, 60);
    preferences.getString(KEY_IMAGE_SELECT, config->image_selection_mode, CONFIG_IMAGE_SELECTION_MODE_MAX_LEN);
    if (strlen(config->image_selection_mode) == 0) {
        strlcpy(config->image_selection_mode, "random", CONFIG_IMAGE_SELECTION_MODE_MAX_LEN);
    }
    config->long_press_ms = preferences.getUShort(KEY_LONG_PRESS, 1500);
    config->always_on = preferences.getBool(KEY_ALWAYS_ON, false);

    // Load MQTT settings (all optional)
    preferences.getString(KEY_MQTT_HOST, config->mqtt_host, CONFIG_MQTT_HOST_MAX_LEN);
    config->mqtt_port = preferences.getUShort(KEY_MQTT_PORT, 0);
    preferences.getString(KEY_MQTT_USER, config->mqtt_username, CONFIG_MQTT_USERNAME_MAX_LEN);
    preferences.getString(KEY_MQTT_PASS, config->mqtt_password, CONFIG_MQTT_PASSWORD_MAX_LEN);
    config->mqtt_interval_seconds = preferences.getUShort(KEY_MQTT_INTERVAL, 0);

    // Azure Blob pull-on-wake
    preferences.getString(KEY_BLOB_SAS_URL, config->blob_sas_url, CONFIG_BLOB_SAS_URL_MAX_LEN);
    
    // Load display settings
    config->backlight_brightness = preferences.getUChar(KEY_BACKLIGHT_BRIGHTNESS, 100);
    LOGI("Config", "Loaded brightness: %d%%", config->backlight_brightness);

    // Load Basic Auth settings
    config->basic_auth_enabled = preferences.getBool(KEY_BASIC_AUTH_ENABLED, false);
    preferences.getString(KEY_BASIC_AUTH_USER, config->basic_auth_username, CONFIG_BASIC_AUTH_USERNAME_MAX_LEN);
    preferences.getString(KEY_BASIC_AUTH_PASS, config->basic_auth_password, CONFIG_BASIC_AUTH_PASSWORD_MAX_LEN);

    config->magic = magic;
    
    preferences.end();
    
    // Validate loaded config
    if (!config_manager_is_valid(config)) {
        LOGE("Config", "Invalid config");
        return false;
    }
    
    config_manager_print(config);
    LOGI("Config", "Load complete");
    return true;
}

// Save configuration to NVS
bool config_manager_save(const DeviceConfig *config) {
    if (!config) {
        LOGE("Config", "Save failed: NULL pointer");
        return false;
    }
    
    if (!config_manager_is_valid(config)) {
        LOGE("Config", "Save failed: Invalid config");
        return false;
    }

    LOGI("Config", "Save start");
    
    preferences.begin(CONFIG_NAMESPACE, false); // Read-write mode
    
    // Save WiFi settings
    preferences.putString(KEY_WIFI_SSID, config->wifi_ssid);
    preferences.putString(KEY_WIFI_PASS, config->wifi_password);
    
    // Save device settings
    preferences.putString(KEY_DEVICE_NAME, config->device_name);
    
    // Save fixed IP settings
    preferences.putString(KEY_FIXED_IP, config->fixed_ip);
    preferences.putString(KEY_SUBNET_MASK, config->subnet_mask);
    preferences.putString(KEY_GATEWAY, config->gateway);
    preferences.putString(KEY_DNS1, config->dns1);
    preferences.putString(KEY_DNS2, config->dns2);
    
    // Save dummy setting
    preferences.putString(KEY_DUMMY, config->dummy_setting);

    // Save phase 2 settings
    preferences.putUShort(KEY_SLEEP_TIMEOUT, config->sleep_timeout_seconds);
    preferences.putString(KEY_IMAGE_SELECT, config->image_selection_mode);
    preferences.putUShort(KEY_LONG_PRESS, config->long_press_ms);
    preferences.putBool(KEY_ALWAYS_ON, config->always_on);

    // Save MQTT settings
    preferences.putString(KEY_MQTT_HOST, config->mqtt_host);
    preferences.putUShort(KEY_MQTT_PORT, config->mqtt_port);
    preferences.putString(KEY_MQTT_USER, config->mqtt_username);
    preferences.putString(KEY_MQTT_PASS, config->mqtt_password);
    preferences.putUShort(KEY_MQTT_INTERVAL, config->mqtt_interval_seconds);

    // Azure Blob pull-on-wake
    preferences.putString(KEY_BLOB_SAS_URL, config->blob_sas_url);
    
    // Save display settings
    LOGI("Config", "Saving brightness: %d%%", config->backlight_brightness);
    preferences.putUChar(KEY_BACKLIGHT_BRIGHTNESS, config->backlight_brightness);

    // Save Basic Auth settings
    preferences.putBool(KEY_BASIC_AUTH_ENABLED, config->basic_auth_enabled);
    preferences.putString(KEY_BASIC_AUTH_USER, config->basic_auth_username);
    preferences.putString(KEY_BASIC_AUTH_PASS, config->basic_auth_password);

    // Save magic number last (indicates valid config)
    preferences.putUInt(KEY_MAGIC, CONFIG_MAGIC);
    
    preferences.end();
    
    config_manager_print(config);
    LOGI("Config", "Save complete");
    return true;
}

// Reset configuration (erase from NVS)
bool config_manager_reset() {
    LOGI("Config", "Reset start");
    
    preferences.begin(CONFIG_NAMESPACE, false);
    bool success = preferences.clear();
    preferences.end();
    
    if (success) {
        LOGI("Config", "Reset complete");
    } else {
        LOGE("Config", "Failed to reset");
    }
    
    return success;
}

// Check if configuration is valid
bool config_manager_is_valid(const DeviceConfig *config) {
    if (!config) return false;
    if (config->magic != CONFIG_MAGIC) return false;
    if (strlen(config->wifi_ssid) == 0) return false;
    if (strlen(config->device_name) == 0) return false;

    if (config->basic_auth_enabled) {
        if (strlen(config->basic_auth_username) == 0) return false;
        if (strlen(config->basic_auth_password) == 0) return false;
    }
    return true;
}

// Print configuration (for debugging)
void config_manager_print(const DeviceConfig *config) {
    if (!config) return;
    
    LOGI("Config", "Device: %s", config->device_name);
    
    // Show sanitized name for mDNS
    char sanitized[CONFIG_DEVICE_NAME_MAX_LEN];
    config_manager_sanitize_device_name(config->device_name, sanitized, CONFIG_DEVICE_NAME_MAX_LEN);
    LOGI("Config", "mDNS: %s.local", sanitized);
    
    LOGI("Config", "WiFi SSID: %s", config->wifi_ssid);
    LOGI("Config", "WiFi Pass: %s", strlen(config->wifi_password) > 0 ? "***" : "(none)");

    LOGI("Config", "Sleep timeout: %us", (unsigned)config->sleep_timeout_seconds);
    LOGI("Config", "Image selection: %s", config->image_selection_mode);
    LOGI("Config", "Long press: %ums", (unsigned)config->long_press_ms);
    LOGI("Config", "Always-on: %s", config->always_on ? "enabled" : "disabled");
    
    if (strlen(config->fixed_ip) > 0) {
        LOGI("Config", "IP: %s", config->fixed_ip);
        LOGI("Config", "Subnet: %s", config->subnet_mask);
        LOGI("Config", "Gateway: %s", config->gateway);
        LOGI("Config", "DNS: %s, %s", config->dns1, strlen(config->dns2) > 0 ? config->dns2 : "(none)");
    } else {
        LOGI("Config", "IP: DHCP");
    }

    LOGI("Config", "Blob SAS URL: %s", strlen(config->blob_sas_url) > 0 ? "set" : "(none)");

#if HAS_MQTT
    if (strlen(config->mqtt_host) > 0) {
        uint16_t port = config->mqtt_port > 0 ? config->mqtt_port : 1883;
        if (config->mqtt_interval_seconds > 0) {
            LOGI("Config", "MQTT: %s:%d (%ds)", config->mqtt_host, port, config->mqtt_interval_seconds);
        } else {
            LOGI("Config", "MQTT: %s:%d (publish disabled)", config->mqtt_host, port);
        }
        LOGI("Config", "MQTT User: %s", strlen(config->mqtt_username) > 0 ? config->mqtt_username : "(none)");
        LOGI("Config", "MQTT Pass: %s", strlen(config->mqtt_password) > 0 ? "***" : "(none)");
    } else {
        LOGI("Config", "MQTT: disabled");
    }
#else
    // MQTT config can still exist in NVS, but the firmware has MQTT support compiled out.
    LOGI("Config", "MQTT: disabled (feature not compiled into firmware)");
#endif
}
