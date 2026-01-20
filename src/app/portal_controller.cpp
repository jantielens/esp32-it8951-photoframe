#include "portal_controller.h"

#include "sd_storage_service.h"
#include "web_portal.h"
#include "web_portal_render_control.h"
#include "log_manager.h"
#if HAS_DISPLAY
#include "display_manager.h"
#endif

#include <WiFi.h>
#include <ESPmDNS.h>

namespace {
static bool init_sd_for_portal(SPIClass &spi, const SdCardPins &pins, uint32_t frequency_hz) {
    LOGI("Portal", "SD init start freq=%lu", (unsigned long)frequency_hz);
#if HAS_DISPLAY
    display_manager_set_splash_status("SD init...");
    display_manager_render_now();
#endif
    if (!sd_storage_configure(spi, pins, frequency_hz)) {
        LOGE("Portal", "SD configure failed");
#if HAS_DISPLAY
        display_manager_set_splash_status("SD failed");
        display_manager_render_now();
#endif
        return false;
    }

    if (!sd_storage_ensure_ready()) {
        delay(200);
        if (!sd_storage_ensure_ready()) {
            LOGE("Portal", "SD init failed after retry");
#if HAS_DISPLAY
            display_manager_set_splash_status("SD failed");
            display_manager_render_now();
#endif
            return false;
        }
    }
    LOGI("Portal", "SD init OK");
#if HAS_DISPLAY
    display_manager_set_splash_status("SD ready");
    display_manager_render_now();
#endif
    return true;
}

static bool ensure_sd_ready(SPIClass &spi, const SdCardPins &pins, uint32_t frequency_hz) {
    if (!init_sd_for_portal(spi, pins, frequency_hz)) {
        LOGW("SD", "Init failed (portal mode) - SD APIs unavailable");
        return false;
    }
    return true;
}

static bool connect_wifi_simple(const DeviceConfig &config) {
    if (strlen(config.wifi_ssid) == 0) return false;

    LOGI("WiFi", "Connect start (ssid set)");
#if HAS_DISPLAY
    display_manager_set_splash_status("Connecting to WiFi...");
    display_manager_render_now();
#endif
    WiFi.mode(WIFI_STA);
    WiFi.begin(config.wifi_ssid, config.wifi_password);

    for (int attempt = 0; attempt < WIFI_MAX_ATTEMPTS; attempt++) {
        const unsigned long start = millis();
        while (millis() - start < 3000) {
            if (WiFi.status() == WL_CONNECTED) {
                LOGI("WiFi", "Connected: %s", WiFi.localIP().toString().c_str());
#if HAS_DISPLAY
                String status = "WiFi connected: ";
                status += WiFi.localIP().toString();
                display_manager_set_splash_status(status.c_str());
                display_manager_render_now();
#endif
                return true;
            }
            delay(100);
        }
        LOGW("WiFi", "Connect attempt %d/%d failed", attempt + 1, WIFI_MAX_ATTEMPTS);
#if HAS_DISPLAY
        char status[64];
        snprintf(status, sizeof(status), "WiFi failed %d/%d", attempt + 1, WIFI_MAX_ATTEMPTS);
        display_manager_set_splash_status(status);
        display_manager_render_now();
#endif
    }

    LOGW("WiFi", "Connect failed (max attempts)");
    return false;
}

static void start_mdns_simple(const DeviceConfig &config) {
    char sanitized[CONFIG_DEVICE_NAME_MAX_LEN];
    config_manager_sanitize_device_name(config.device_name, sanitized, sizeof(sanitized));
    if (strlen(sanitized) == 0) return;

    if (MDNS.begin(sanitized)) {
        MDNS.addService("http", "tcp", 80);
        LOGI("mDNS", "Name: %s.local", sanitized);
    } else {
        LOGW("mDNS", "Start failed");
    }
}
}

void portal_controller_start(DeviceConfig &config, bool config_loaded, SPIClass &spi, const SdCardPins &pins, uint32_t frequency_hz) {
    LOGI("Portal", "Portal start");

    if (!ensure_sd_ready(spi, pins, frequency_hz)) {
        // SD APIs will be unavailable; keep portal running anyway.
    }

    if (!config_loaded || strlen(config.wifi_ssid) == 0) {
        LOGI("WiFi", "No config - starting AP mode");
        web_portal_start_ap();
    } else if (connect_wifi_simple(config)) {
        start_mdns_simple(config);
    } else {
        LOGW("WiFi", "Connect failed - fallback to AP mode");
        web_portal_start_ap();
    }

    LOGI("Portal", "Init web portal");
#if HAS_DISPLAY
    display_manager_set_splash_status("Portal ready");
    display_manager_render_now();
#endif
    web_portal_init(&config);
}

void portal_controller_tick() {
    web_portal_handle();
    sd_storage_purge_jobs();

    #if HAS_DISPLAY
    display_manager_tick();
    #endif

    static bool last_paused = false;
    const bool paused = web_portal_render_is_paused();
    if (paused != last_paused) {
        LOGI("Portal", "Render pause state=%s", paused ? "paused" : "active");
        last_paused = paused;
    }
}

bool portal_controller_is_paused() {
    return web_portal_render_is_paused();
}
