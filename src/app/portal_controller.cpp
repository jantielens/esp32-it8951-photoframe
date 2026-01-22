#include "portal_controller.h"

#include "sd_storage_service.h"
#include "web_portal.h"
#include "web_portal_render_control.h"
#include "log_manager.h"
#include "display_manager.h"
#include "rtc_state.h"

#include <WiFi.h>
#include <ESPmDNS.h>
#include <lwip/netif.h>
#include <esp_netif.h>

namespace {
static bool init_sd_for_portal(SPIClass &spi, const SdCardPins &pins, uint32_t frequency_hz) {
    LOGI("Portal", "SD init start freq=%lu", (unsigned long)frequency_hz);
    display_manager_set_splash_status("SD init...");
    display_manager_render_now();
    if (!sd_storage_configure(spi, pins, frequency_hz)) {
        LOGE("Portal", "SD configure failed");
        display_manager_set_splash_status("SD failed");
        display_manager_render_now();
        return false;
    }

    if (!sd_storage_ensure_ready()) {
        delay(200);
        if (!sd_storage_ensure_ready()) {
            LOGE("Portal", "SD init failed after retry");
            display_manager_set_splash_status("SD failed");
            display_manager_render_now();
            return false;
        }
    }
    LOGI("Portal", "SD init OK");
    display_manager_set_splash_status("SD ready");
    display_manager_render_now();
    return true;
}

static bool ensure_sd_ready(SPIClass &spi, const SdCardPins &pins, uint32_t frequency_hz) {
    if (!init_sd_for_portal(spi, pins, frequency_hz)) {
        LOGW("SD", "Init failed (portal mode) - SD APIs unavailable");
        return false;
    }
    return true;
}

static const char* wl_status_str(wl_status_t status) {
    switch (status) {
        case WL_NO_SSID_AVAIL: return "SSID not found";
        case WL_CONNECT_FAILED: return "Connect failed";
        case WL_CONNECTION_LOST: return "Connection lost";
        case WL_DISCONNECTED: return "Disconnected";
        case WL_IDLE_STATUS: return "Idle";
        case WL_SCAN_COMPLETED: return "Scan done";
        case WL_CONNECTED: return "Connected";
        default: return "Unknown";
    }
}

static void format_bssid(const uint8_t *bssid, char *out, size_t out_len) {
    if (!out || out_len < 18) return;
    if (!bssid) {
        snprintf(out, out_len, "--:--:--:--:--:--");
        return;
    }
    snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X",
        bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
}

static void wifi_set_hostname_from_config(const DeviceConfig &config) {
    char sanitized[CONFIG_DEVICE_NAME_MAX_LEN];
    config_manager_sanitize_device_name(config.device_name, sanitized, sizeof(sanitized));
    if (strlen(sanitized) == 0) return;

    WiFi.setHostname(sanitized);

    // Also set via esp_netif API for compatibility.
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL) {
        esp_netif_set_hostname(netif, sanitized);
    }
}

static bool wifi_apply_static_ip_if_configured(const DeviceConfig &config) {
    if (strlen(config.fixed_ip) == 0) return true;

    IPAddress local_ip, gateway, subnet, dns1, dns2;

    if (!local_ip.fromString(config.fixed_ip)) return false;
    if (!subnet.fromString(config.subnet_mask)) return false;
    if (!gateway.fromString(config.gateway)) return false;

    if (strlen(config.dns1) > 0) {
        dns1.fromString(config.dns1);
    } else {
        dns1 = gateway;
    }

    if (strlen(config.dns2) > 0) {
        dns2.fromString(config.dns2);
    } else {
        dns2 = IPAddress(0, 0, 0, 0);
    }

    return WiFi.config(local_ip, gateway, subnet, dns1, dns2);
}

static void wifi_stack_quick_reset(bool robust) {
    WiFi.persistent(false);
    WiFi.disconnect(true);
    delay(robust ? 100 : 30);
    WiFi.mode(WIFI_OFF);
    delay(robust ? 500 : 150);
    WiFi.mode(WIFI_STA);
    delay(robust ? 100 : 30);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
}

static void wifi_stack_prepare_for_connect() {
    // These are also applied in wifi_stack_quick_reset(), but for SleepCycle we
    // want the "fast path" to have the same sane defaults without having to
    // toggle the radio off/on.
    WiFi.persistent(false);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
}

static bool select_strongest_ap_scan(const char *target_ssid, uint8_t out_bssid[6], uint8_t *out_channel, int8_t *out_rssi) {
    if (!target_ssid || strlen(target_ssid) == 0) return false;
    WiFi.scanDelete();

    const int16_t n = WiFi.scanNetworks();
    if (n <= 0) {
        WiFi.scanDelete();
        return false;
    }

    int best_index = -1;
    int best_rssi = -1000;
    for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i) == target_ssid) {
            const int rssi = WiFi.RSSI(i);
            if (best_index < 0 || rssi > best_rssi) {
                best_index = i;
                best_rssi = rssi;
            }
        }
    }

    if (best_index < 0) {
        WiFi.scanDelete();
        return false;
    }

    const uint8_t *best_bssid_ptr = WiFi.BSSID(best_index);
    const int best_channel = WiFi.channel(best_index);
    if (!best_bssid_ptr || best_channel <= 0) {
        WiFi.scanDelete();
        return false;
    }

    memcpy(out_bssid, best_bssid_ptr, 6);
    if (out_channel) *out_channel = (uint8_t)best_channel;
    if (out_rssi) *out_rssi = (int8_t)best_rssi;

    WiFi.scanDelete();
    return true;
}

struct WifiConnectOpts {
    const char *reason = nullptr;
    bool show_status = false;
    bool allow_scan = false;
    bool allow_reset_escalation = false;
    uint32_t budget_ms = 6000;
    // If true, allow a single WiFi reset when we appear stuck in WL_IDLE_STATUS.
    bool allow_idle_stall_reset = false;
    // Minimum time to wait after WiFi.begin() before treating WL_IDLE_STATUS as a stall.
    uint32_t idle_stall_reset_after_ms = 3500;
};

static bool wifi_connect_internal(const DeviceConfig &config, const WifiConnectOpts &opts) {
    if (strlen(config.wifi_ssid) == 0) return false;
    if (WiFi.status() == WL_CONNECTED) return true;

    const unsigned long start_ms = millis();

    if (opts.show_status) {
        display_manager_set_splash_status("Connecting to WiFi...");
        display_manager_render_now();
    }

    WiFi.mode(WIFI_STA);
    wifi_stack_prepare_for_connect();
    wifi_set_hostname_from_config(config);

    const bool wants_static = (strlen(config.fixed_ip) > 0);
    if (wants_static) {
        if (!wifi_apply_static_ip_if_configured(config)) {
            // Invalid static config: fall back to DHCP.
            LOGW("WiFi", "%s: invalid static IP config; using DHCP", opts.reason ? opts.reason : "WiFi");
        }
    }

    uint8_t bssid[6];
    uint8_t channel = 0;
    int8_t rssi = -127;
    bool have_hint = rtc_wifi_state_get_best_ap(config.wifi_ssid, bssid, &channel);

    if (opts.allow_scan) {
        uint8_t scan_bssid[6];
        uint8_t scan_channel = 0;
        int8_t scan_rssi = -127;
        if (select_strongest_ap_scan(config.wifi_ssid, scan_bssid, &scan_channel, &scan_rssi)) {
            memcpy(bssid, scan_bssid, 6);
            channel = scan_channel;
            rssi = scan_rssi;
            have_hint = true;
            rtc_wifi_state_set_best_ap(config.wifi_ssid, bssid, channel, rssi);
        }
    }

    char bssid_str[18];
    format_bssid(have_hint ? bssid : nullptr, bssid_str, sizeof(bssid_str));
    LOGI("WiFi", "%s: connect start (hint=%s ch=%u static=%s)",
        opts.reason ? opts.reason : "WiFi",
        have_hint ? bssid_str : "none",
        (unsigned)channel,
        wants_static ? "yes" : "no");

    if (have_hint) {
        WiFi.begin(config.wifi_ssid, config.wifi_password, channel, bssid);
    } else {
        WiFi.begin(config.wifi_ssid, config.wifi_password);
    }

    const unsigned long deadline_ms = start_ms + opts.budget_ms;
    bool did_reset = false;
    unsigned long begin_ms = millis();
    wl_status_t last_status = WiFi.status();
    unsigned long last_status_log_ms = 0;

    while (millis() < deadline_ms) {
        if (WiFi.status() == WL_CONNECTED) {
            const uint8_t *conn_bssid = WiFi.BSSID();
            const uint8_t conn_channel = (uint8_t)WiFi.channel();
            const int8_t conn_rssi = (int8_t)WiFi.RSSI();
            if (conn_bssid && conn_channel > 0) {
                rtc_wifi_state_set_best_ap(config.wifi_ssid, conn_bssid, conn_channel, conn_rssi);
            }

            if (opts.show_status) {
                String status = "WiFi connected: ";
                status += WiFi.localIP().toString();
                display_manager_set_splash_status(status.c_str());
                display_manager_render_now();
            }

            char conn_bssid_str[18];
            format_bssid(conn_bssid, conn_bssid_str, sizeof(conn_bssid_str));
            LOGI("WiFi", "%s: connected %s rssi=%d bssid=%s ch=%u",
                opts.reason ? opts.reason : "WiFi",
                WiFi.localIP().toString().c_str(),
                (int)WiFi.RSSI(),
                conn_bssid_str,
                (unsigned)WiFi.channel());
            return true;
        }

        const unsigned long now_ms = millis();
        const wl_status_t st = WiFi.status();
        if (st != last_status || last_status_log_ms == 0 || (now_ms - last_status_log_ms) >= 1000) {
            LOGI("WiFi", "%s: status %s/%d", opts.reason ? opts.reason : "WiFi", wl_status_str(st), (int)st);
            last_status = st;
            last_status_log_ms = now_ms;
        }

        if (opts.allow_reset_escalation && !did_reset) {
            const unsigned long elapsed_since_begin = now_ms - begin_ms;
            const unsigned long remaining = (deadline_ms > now_ms) ? (deadline_ms - now_ms) : 0;

            const bool is_hard_fail = (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL);
            const bool is_idle_stall = (opts.allow_idle_stall_reset && st == WL_IDLE_STATUS && elapsed_since_begin >= opts.idle_stall_reset_after_ms);

            if ((is_hard_fail || is_idle_stall) && remaining > 1500) {
                LOGW("WiFi", "%s: escalating to WiFi reset (%s)",
                    opts.reason ? opts.reason : "WiFi",
                    is_hard_fail ? "hard-fail" : "idle-stall");
                wifi_stack_quick_reset(/*robust=*/false);
                wifi_stack_prepare_for_connect();
                did_reset = true;
                begin_ms = millis();

                if (have_hint) {
                    WiFi.begin(config.wifi_ssid, config.wifi_password, channel, bssid);
                } else {
                    WiFi.begin(config.wifi_ssid, config.wifi_password);
                }
            }
        }

        delay(50);
    }

    const wl_status_t final_status = WiFi.status();
    LOGW("WiFi", "%s: connect timeout (%s/%d)",
        opts.reason ? opts.reason : "WiFi",
        wl_status_str(final_status),
        (int)final_status);
    return false;

}

} // namespace

bool wifi_connect_fast_sleepcycle(const DeviceConfig &config, const char *reason, uint32_t budget_ms, bool show_status) {
    WifiConnectOpts opts;
    opts.reason = reason;
    opts.show_status = show_status;
    opts.allow_scan = false;
    opts.allow_reset_escalation = true;
    opts.allow_idle_stall_reset = true;
    opts.idle_stall_reset_after_ms = (budget_ms >= 4500) ? 3500 : (budget_ms / 2);
    opts.budget_ms = budget_ms;
    return wifi_connect_internal(config, opts);
}

bool wifi_connect_robust_portal(const DeviceConfig &config, const char *reason, bool show_status) {
    WifiConnectOpts opts;
    opts.reason = reason;
    opts.show_status = show_status;
    opts.allow_scan = true;
    opts.allow_reset_escalation = true;
    // In portal mode we allow scan + longer budget; idle is normal while connecting.
    opts.allow_idle_stall_reset = false;
    opts.budget_ms = 15000;
    return wifi_connect_internal(config, opts);
}

void wifi_start_mdns(const DeviceConfig &config) {
    char sanitized[CONFIG_DEVICE_NAME_MAX_LEN];
    config_manager_sanitize_device_name(config.device_name, sanitized, sizeof(sanitized));
    if (strlen(sanitized) == 0) {
        LOGW("mDNS", "No device name set; skipping mDNS");
        return;
    }

    // Keep it simple and resilient: restarting mDNS is cheap and avoids
    // "already started" edge cases.
    MDNS.end();
    if (!MDNS.begin(sanitized)) {
        LOGW("mDNS", "Failed to start (%s.local)", sanitized);
        return;
    }

    MDNS.addService("http", "tcp", 80);
    LOGI("mDNS", "Started %s.local", sanitized);
}

void portal_controller_start(DeviceConfig &config, bool config_loaded, SPIClass &spi, const SdCardPins &pins, uint32_t frequency_hz) {
    LOGI("Portal", "Portal start");

    if (!ensure_sd_ready(spi, pins, frequency_hz)) {
        // SD APIs will be unavailable; keep portal running anyway.
    }

    if (!config_loaded || strlen(config.wifi_ssid) == 0) {
        LOGI("WiFi", "No config - starting AP mode");
        web_portal_start_ap();
    } else if (wifi_connect_robust_portal(config, "Portal", /*show_status=*/true)) {
        wifi_start_mdns(config);
    } else {
        LOGW("WiFi", "Connect failed - fallback to AP mode");
        web_portal_start_ap();
    }

    LOGI("Portal", "Init web portal");
    display_manager_set_splash_status("Portal ready");
    display_manager_render_now();
    web_portal_init(&config);
}

void portal_controller_tick() {
    web_portal_handle();
    sd_storage_purge_jobs();
    display_manager_tick();

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
