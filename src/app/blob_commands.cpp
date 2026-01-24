#include "blob_commands.h"

#include "azure_blob_client.h"
#include "log_manager.h"
#include "sd_storage_service.h"
#include "rtc_state.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <vector>

namespace {
static constexpr uint32_t kBlobHttpTimeoutMs = 15000;
static constexpr uint32_t kBlobHttpRetryDelayMs = 1000;
static constexpr uint8_t kBlobHttpRetries = 3;
static constexpr uint16_t kBlobListMaxResults = 50;

static constexpr uint8_t kMaxCommandsPerWake = 10;
static constexpr size_t kMaxCommandNameLen = 127;
static constexpr size_t kMaxCommandJsonBytes = 4096;

static constexpr uint32_t kSdJobTimeoutMs = 180000;

static bool starts_with(const String &s, const char *prefix) {
    if (!prefix) return false;
    const size_t n = strlen(prefix);
    if (s.length() < n) return false;
    return strncmp(s.c_str(), prefix, n) == 0;
}

static bool is_valid_command_blob_name(const String &name) {
    if (name.length() == 0 || name.length() > kMaxCommandNameLen) return false;
    if (name.indexOf('\\') >= 0) return false;
    if (name.indexOf("..") >= 0) return false;

    if (!starts_with(name, "commands/")) return false;

    String lower = name;
    lower.toLowerCase();
    if (!lower.endsWith(".json")) return false;

    // Require exactly one '/' separator: commands/<file>.json
    const int first = name.indexOf('/');
    if (first < 0) return false;
    if (name.lastIndexOf('/') != first) return false;

    return true;
}

static bool is_valid_queue_g4_path(const String &path) {
    if (path.length() == 0 || path.length() > kMaxCommandNameLen) return false;
    if (path.indexOf('\\') >= 0) return false;
    if (path.indexOf("..") >= 0) return false;

    String lower = path;
    lower.toLowerCase();
    if (!lower.endsWith(".g4")) return false;

    const int slash_pos = path.indexOf('/');
    if (slash_pos < 0) return false;
    if (path.lastIndexOf('/') != slash_pos) return false;

    return path.startsWith("queue-permanent/") || path.startsWith("queue-temporary/");
}

static bool download_command_json_bounded(const AzureSasUrlParts &sas, const String &blob_name, uint8_t **out_buf, size_t *out_size, int *out_http_code) {
    return azure_blob_download_to_buffer_bounded(
        sas,
        blob_name,
        kMaxCommandJsonBytes,
        out_buf,
        out_size,
        kBlobHttpTimeoutMs,
        kBlobHttpRetries,
        kBlobHttpRetryDelayMs,
        out_http_code
    );
}

static bool delete_command_blob(const AzureSasUrlParts &sas, const String &blob_name) {
    return azure_blob_delete(sas, blob_name, kBlobHttpTimeoutMs, kBlobHttpRetries, kBlobHttpRetryDelayMs);
}

static bool wait_sd_job(uint32_t job_id, const char *label) {
    if (job_id == 0) return false;

    const uint32_t start = millis();
    while (millis() - start < kSdJobTimeoutMs) {
        SdJobInfo info = {};
        if (!sd_storage_get_job(job_id, &info)) {
            delay(50);
            continue;
        }

        if (info.state == SdJobState::Done) {
            if (!info.success) {
                LOGW("Cmd", "%s failed: %s", label ? label : "job", info.message);
            }
            return info.success;
        }

        if (info.state == SdJobState::Error) {
            LOGW("Cmd", "%s error: %s", label ? label : "job", info.message);
            return false;
        }

        delay(50);
    }

    LOGW("Cmd", "%s timeout", label ? label : "job");
    return false;
}

static String derive_all_g4_blob_name(const String &queue_name) {
    if (starts_with(queue_name, "queue-permanent/")) {
        return String("all/permanent/") + queue_name.substring(strlen("queue-permanent/"));
    }
    if (starts_with(queue_name, "queue-temporary/")) {
        return String("all/temporary/") + queue_name.substring(strlen("queue-temporary/"));
    }
    return String();
}

static String derive_thumb_blob_name(const String &queue_g4_name) {
    // queue-permanent/<x>.g4 -> all/permanent/<x>__thumb.jpg
    // queue-temporary/<x>.g4 -> all/temporary/<x>__thumb.jpg
    if (queue_g4_name.length() < 3) return String();
    String lower = queue_g4_name;
    lower.toLowerCase();
    if (!lower.endsWith(".g4")) return String();

    const String base = queue_g4_name.substring(0, queue_g4_name.length() - 3);
    if (starts_with(base, "queue-permanent/")) {
        return String("all/permanent/") + base.substring(strlen("queue-permanent/")) + "__thumb.jpg";
    }
    if (starts_with(base, "queue-temporary/")) {
        return String("all/temporary/") + base.substring(strlen("queue-temporary/")) + "__thumb.jpg";
    }
    return String();
}

static bool list_prefix_sorted(const AzureSasUrlParts &sas, const String &prefix, std::vector<String> &out_names) {
    out_names.clear();

    String marker;
    uint32_t pages = 0;
    while (true) {
        pages++;
        std::vector<String> names;
        String next_marker;
        if (!azure_blob_list_page(sas, prefix, marker, kBlobListMaxResults, names, next_marker, kBlobHttpTimeoutMs, kBlobHttpRetries, kBlobHttpRetryDelayMs)) {
            LOGW("Cmd", "List failed (prefix=%s page=%lu)", prefix.c_str(), (unsigned long)pages);
            return false;
        }

        out_names.insert(out_names.end(), names.begin(), names.end());

        if (next_marker.length() == 0) break;
        marker = next_marker;
    }

    std::sort(out_names.begin(), out_names.end(), [](const String &a, const String &b) {
        return a.compareTo(b) < 0;
    });
    return true;
}

static bool parse_uint32_arg(const JsonVariantConst &v, uint32_t *out) {
    if (!out) return false;
    if (v.is<uint32_t>()) {
        *out = v.as<uint32_t>();
        return true;
    }
    if (v.is<int>()) {
        const int n = v.as<int>();
        if (n < 0) return false;
        *out = (uint32_t)n;
        return true;
    }
    if (v.is<const char *>()) {
        const char *s = v.as<const char *>();
        if (!s || !*s) return false;
        char *end = nullptr;
        const unsigned long n = strtoul(s, &end, 10);
        if (!end || *end != '\0') return false;
        *out = (uint32_t)n;
        return true;
    }
    return false;
}

static bool handle_set_rotation_interval(DeviceConfig &config, JsonObjectConst args) {
    uint32_t seconds = 0;
    if (!parse_uint32_arg(args["seconds"], &seconds) || seconds == 0 || seconds > 86400) {
        LOGW("Cmd", "Invalid set_rotation_interval.seconds");
        return false;
    }

    config.sleep_timeout_seconds = (uint16_t)seconds;
    if (!config_manager_save(&config)) {
        LOGW("Cmd", "Failed to save config for set_rotation_interval");
        return false;
    }

    LOGI("Cmd", "Updated sleep_timeout_seconds=%lu", (unsigned long)seconds);
    return true;
}

static bool handle_delete_photo(const AzureSasUrlParts &sas, SPIClass &spi, const SdCardPins &pins, uint32_t frequency_hz, JsonObjectConst args) {
    const char *path = args["path"] | "";
    if (!path || !*path) {
        LOGW("Cmd", "delete_photo missing args.path");
        return false;
    }

    const String queue_name(path);
    if (queue_name.length() > kMaxCommandNameLen) {
        LOGW("Cmd", "delete_photo path too long");
        return false;
    }

    if (!is_valid_queue_g4_path(queue_name)) {
        LOGW("Cmd", "delete_photo invalid args.path: %s", queue_name.c_str());
        return false;
    }

    // Configure SD service if possible (best-effort).
    if (!sd_storage_configure(spi, pins, frequency_hz)) {
        LOGW("Cmd", "SD init failed for delete_photo (continuing cloud delete only)");
    }

    // 1) Delete on SD (logical name).
    const uint32_t del_job = sd_storage_enqueue_delete(queue_name.c_str());
    if (del_job != 0) {
        (void)wait_sd_job(del_job, "SD delete");
    }

    // 2) Delete from Azure archive if derivable.
    const String all_g4 = derive_all_g4_blob_name(queue_name);
    const String thumb = derive_thumb_blob_name(queue_name);
    if (all_g4.length() > 0) {
        if (thumb.length() > 0) {
            LOGI("Cmd", "delete_photo cloud delete: g4=%s thumb=%s", all_g4.c_str(), thumb.c_str());
        } else {
            LOGI("Cmd", "delete_photo cloud delete: g4=%s", all_g4.c_str());
        }

        const bool ok_g4 = azure_blob_delete(sas, all_g4, kBlobHttpTimeoutMs, kBlobHttpRetries, kBlobHttpRetryDelayMs);
        if (!ok_g4) {
            LOGW("Cmd", "delete_photo cloud delete failed: %s", all_g4.c_str());
        }

        if (thumb.length() > 0) {
            const bool ok_thumb = azure_blob_delete(sas, thumb, kBlobHttpTimeoutMs, kBlobHttpRetries, kBlobHttpRetryDelayMs);
            if (!ok_thumb) {
                LOGW("Cmd", "delete_photo cloud delete failed: %s", thumb.c_str());
            }
        }
    }

    LOGI("Cmd", "delete_photo done: %s", queue_name.c_str());
    return true;
}

static bool handle_resync_from_cloud(SPIClass &spi, const SdCardPins &pins, uint32_t frequency_hz, const char *sas_url) {
    if (!sas_url || !*sas_url) return false;

    if (!sd_storage_configure(spi, pins, frequency_hz)) {
        LOGW("Cmd", "SD init failed for resync_from_cloud");
        return false;
    }

    const uint32_t job_id = sd_storage_enqueue_sync_from_azure(sas_url);
    if (job_id == 0) {
        LOGW("Cmd", "Failed to enqueue sync_from_azure");
        return false;
    }

    return wait_sd_job(job_id, "sync_from_azure");
}

static bool handle_clean_all_content(const AzureSasUrlParts &sas, SPIClass &spi, const SdCardPins &pins, uint32_t frequency_hz) {
    // 1) Best-effort delete SD queue directories by listing known names.
    if (sd_storage_configure(spi, pins, frequency_hz)) {
        const uint32_t list_job = sd_storage_enqueue_list();
        if (list_job != 0) {
            const bool listed = wait_sd_job(list_job, "SD list");
            if (listed) {
                std::vector<String> names;
                if (sd_storage_get_job_names(list_job, names)) {
                    for (const auto &name : names) {
                        if (!(name.startsWith("queue-permanent/") || name.startsWith("queue-temporary/"))) continue;
                        const uint32_t del_job = sd_storage_enqueue_delete(name.c_str());
                        if (del_job != 0) {
                            (void)wait_sd_job(del_job, "SD delete");
                        }
                    }
                }
            }
        }
    } else {
        LOGW("Cmd", "SD init failed for clean_all_content; skipping SD cleanup");
    }

    // 2) Best-effort delete Azure known prefixes.
    const char *prefixes[] = {
        "queue-temporary/",
        "queue-permanent/",
        "all/temporary/",
        "all/permanent/",
        "commands/",
    };

    for (size_t i = 0; i < (sizeof(prefixes) / sizeof(prefixes[0])); i++) {
        std::vector<String> names;
        if (!list_prefix_sorted(sas, String(prefixes[i]), names)) {
            continue;
        }
        for (const auto &name : names) {
            (void)azure_blob_delete(sas, name, kBlobHttpTimeoutMs, kBlobHttpRetries, kBlobHttpRetryDelayMs);
        }
    }

    LOGI("Cmd", "clean_all_content done");
    return true;
}

static bool execute_command(
    const AzureSasUrlParts &sas,
    const String &command_blob,
    DeviceConfig &config,
    SPIClass &spi,
    const SdCardPins &pins,
    uint32_t frequency_hz,
    const JsonDocument &doc,
    BlobCommandActions &out_actions
) {
    const int v = doc["v"] | 0;
    if (v != 1) {
        LOGW("Cmd", "Unsupported command version v=%d", v);
        return false;
    }

    const char *op = doc["op"] | "";
    if (!op || !*op) {
        LOGW("Cmd", "Missing op");
        return false;
    }

    const char *id = doc["id"] | "";
    const char *created_at = doc["created_at_utc"] | "";
    LOGI("Cmd", "Exec v=1 op=%s id=%s created=%s", op, (id && *id) ? id : "-", (created_at && *created_at) ? created_at : "-");

    JsonObjectConst args = doc["args"].as<JsonObjectConst>();

    if (strcmp(op, "reboot_device") == 0) {
        out_actions.reboot_now = true;
        out_actions.stop_processing_now = true;
        return true;
    }

    if (strcmp(op, "enable_config_portal") == 0) {
        out_actions.enter_config_portal_now = true;
        out_actions.stop_processing_now = true;
        return true;
    }

    if (strcmp(op, "set_rotation_interval") == 0) {
        return handle_set_rotation_interval(config, args);
    }

    if (strcmp(op, "show_next") == 0) {
        // In sleep-cycle mode, rendering once will already advance to the next image.
        // Optionally override the sleep duration for this cycle.
        const char *path = args["path"] | "";
        if (path && *path) {
            const String queue_path(path);
            if (!is_valid_queue_g4_path(queue_path)) {
                LOGW("Cmd", "show_next invalid args.path: %s", queue_path.c_str());
                return false;
            }
            rtc_image_state_set_priority_image_name(queue_path.c_str());
            LOGI("Cmd", "show_next priority=%s", queue_path.c_str());
        } else {
            LOGI("Cmd", "show_next (no path) -> normal selection");
        }

        uint32_t seconds = 0;
        if (parse_uint32_arg(args["duration_seconds"], &seconds)) {
            if (seconds >= 10 && seconds <= 86400) {
                out_actions.override_sleep_seconds = true;
                out_actions.sleep_seconds = seconds;
                LOGI("Cmd", "show_next duration_seconds=%lu", (unsigned long)seconds);
            } else {
                LOGW("Cmd", "show_next duration_seconds out of range");
                return false;
            }
        }
        out_actions.stop_processing_now = true;
        return true;
    }

    if (strcmp(op, "resync_from_cloud") == 0) {
        if (!handle_resync_from_cloud(spi, pins, frequency_hz, config.blob_sas_url)) {
            return false;
        }
        out_actions.request_resync_from_cloud = true;
        return true;
    }

    if (strcmp(op, "delete_photo") == 0) {
        return handle_delete_photo(sas, spi, pins, frequency_hz, args);
    }

    if (strcmp(op, "clean_all_content") == 0) {
        out_actions.stop_processing_now = true;
        out_actions.skip_render_and_sleep = true;
        return handle_clean_all_content(sas, spi, pins, frequency_hz);
    }

    LOGW("Cmd", "Unknown op=%s", op);
    (void)command_blob;
    return false;
}

} // namespace

bool blob_commands_process(DeviceConfig &config, SPIClass &spi, const SdCardPins &pins, uint32_t frequency_hz, BlobCommandActions &out_actions) {
    if (strlen(config.blob_sas_url) == 0) return false;
    if (WiFi.status() != WL_CONNECTED) return false;

    AzureSasUrlParts sas = {};
    if (!azure_blob_parse_sas_url(config.blob_sas_url, sas)) {
        LOGW("Cmd", "Invalid blob SAS URL");
        return false;
    }

    std::vector<String> names;
    if (!list_prefix_sorted(sas, String("commands/"), names)) {
        return false;
    }

    // Filter, validate and sort.
    std::vector<String> filtered;
    filtered.reserve(names.size());
    for (const auto &name : names) {
        if (is_valid_command_blob_name(name)) {
            filtered.push_back(name);
        }
    }

    if (filtered.empty()) return false;

    std::sort(filtered.begin(), filtered.end(), [](const String &a, const String &b) {
        return a.compareTo(b) < 0;
    });

    uint8_t executed = 0;

    for (const auto &command_blob : filtered) {
        if (executed >= kMaxCommandsPerWake) break;

        uint8_t *buf = nullptr;
        size_t size = 0;
        int http_code = 0;

        LOGI("Cmd", "Fetching %s", command_blob.c_str());

        const bool ok = download_command_json_bounded(sas, command_blob, &buf, &size, &http_code);
        if (!ok || !buf || size == 0) {
            if (buf) heap_caps_free(buf);
            LOGW("Cmd", "Command download failed http=%d name=%s", http_code, command_blob.c_str());
            // If the blob disappeared between list+get, skip it.
            if (http_code == 404) {
                executed++;
                continue;
            }
            // Strict sequential semantics: stop here and retry next wake.
            executed++;
            break;
        }

        // Ensure null-terminated copy for JSON parsing.
        const size_t copy_n = size;
        char *json = (char *)heap_caps_malloc(copy_n + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!json) {
            heap_caps_free(buf);
            LOGW("Cmd", "OOM parsing %s", command_blob.c_str());
            executed++;
            break;
        }
        memcpy(json, buf, copy_n);
        json[copy_n] = '\0';
        heap_caps_free(buf);

        StaticJsonDocument<2048> doc;
        const DeserializationError err = deserializeJson(doc, json);
        heap_caps_free(json);

        if (err) {
            LOGW("Cmd", "JSON parse error: %s (%s)", err.c_str(), command_blob.c_str());
            executed++;
            break;
        }

        const bool exec_ok = execute_command(sas, command_blob, config, spi, pins, frequency_hz, doc, out_actions);
        if (exec_ok) {
            if (!delete_command_blob(sas, command_blob)) {
                LOGW("Cmd", "Delete command failed (will retry next wake): %s", command_blob.c_str());
            } else {
                LOGI("Cmd", "Done: %s", command_blob.c_str());
            }
        } else {
            LOGW("Cmd", "Command failed (kept for retry): %s", command_blob.c_str());
            executed++;
            break;
        }

        executed++;

        // If we need to pivot boot mode (portal / reboot), stop processing.
        if (out_actions.enter_config_portal_now || out_actions.reboot_now) {
            break;
        }

        if (out_actions.stop_processing_now) {
            break;
        }
    }

    return executed > 0;
}
