#include "blob_pull.h"

#include "azure_blob_client.h"

#include "board_config.h"
#include "log_manager.h"
#include "sd_storage_service.h"
#include "rtc_state.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <vector>

namespace {
static constexpr uint32_t kBlobHttpTimeoutMs = 15000;
static constexpr uint32_t kBlobHttpRetryDelayMs = 1000;
static constexpr uint8_t kBlobHttpRetries = 3;
static constexpr uint16_t kBlobListMaxResults = 50;
static constexpr size_t kMaxG4NameLen = 127;
static constexpr uint32_t kBlobUploadJobTimeoutMs = 120000;

static bool name_is_g4(const String &name) {
    if (name.length() < 3) return false;
    String lower = name;
    lower.toLowerCase();
    return lower.endsWith(".g4");
}

static void log_memory_snapshot(const char *label) {
    const size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t heap_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psram_free = 0;
    size_t psram_min = 0;
    if (psramFound()) {
        psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        psram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    }
    LOGI("Blob", "%s mem: heap_free=%lu heap_min=%lu psram_free=%lu psram_min=%lu",
         label,
         (unsigned long)heap_free,
         (unsigned long)heap_min,
         (unsigned long)psram_free,
         (unsigned long)psram_min);
}


static String make_sd_path(const String &name) {
    if (name.length() == 0) return String();
    String path = "/";
    path += name;
    return path;
}

static bool download_blob_to_buffer(const AzureSasUrlParts &sas, const String &name, uint8_t **out_buf, size_t *out_size) {
    log_memory_snapshot("HTTP download");
    return azure_blob_download_to_buffer(sas, name, out_buf, out_size, kBlobHttpTimeoutMs, kBlobHttpRetries, kBlobHttpRetryDelayMs);
}

static bool enqueue_sd_upload_and_wait(const String &name, uint8_t *buffer, size_t size) {
    if (!buffer || size == 0) return false;

    uint32_t job_id = sd_storage_enqueue_upload(name.c_str(), buffer, size);
    if (job_id == 0) {
        LOGW("Blob", "SD upload enqueue failed for %s", name.c_str());
        heap_caps_free(buffer);
        return false;
    }

    const uint32_t start = millis();
    while (millis() - start < kBlobUploadJobTimeoutMs) {
        SdJobInfo info = {};
        if (!sd_storage_get_job(job_id, &info)) {
            delay(50);
            continue;
        }
        if (info.state == SdJobState::Done) {
            if (info.success) {
                LOGI("Blob", "SD upload complete: %s", name.c_str());
                return true;
            }
            LOGW("Blob", "SD upload failed: %s (%s)", name.c_str(), info.message);
            return false;
        }
        if (info.state == SdJobState::Error) {
            LOGW("Blob", "SD upload error: %s (%s)", name.c_str(), info.message);
            return false;
        }
        delay(50);
    }

    LOGW("Blob", "SD upload timeout: %s", name.c_str());
    return false;
}

static bool delete_blob(const AzureSasUrlParts &sas, const String &name) {
    return azure_blob_delete(sas, name, kBlobHttpTimeoutMs, kBlobHttpRetries, kBlobHttpRetryDelayMs);
}

}

bool blob_pull_download_once(const DeviceConfig &config, SPIClass &spi, const SdCardPins &pins, uint32_t frequency_hz) {
    if (strlen(config.blob_sas_url) == 0) return false;

    AzureSasUrlParts sas = {};
    if (!azure_blob_parse_sas_url(config.blob_sas_url, sas)) {
        LOGE("Blob", "Invalid SAS URL (expected https://...?...)");
        return false;
    }

    LOGI("Blob", "Pull-on-wake: starting");

    if (WiFi.status() != WL_CONNECTED) {
        LOGW("Blob", "WiFi not connected; skipping blob pull");
        return false;
    }

    if (!sd_storage_configure(spi, pins, frequency_hz)) {
        LOGE("Blob", "SD service init failed; skipping blob pull");
        return false;
    }

    // Prefix-scoped listing (queue-temporary/ then queue-permanent/) so we never enumerate all/.
    const char *prefixes[] = {"queue-temporary/", "queue-permanent/"};
    for (size_t p = 0; p < (sizeof(prefixes) / sizeof(prefixes[0])); p++) {
        const String prefix(prefixes[p]);
        String marker;
        uint32_t page = 0;

        while (true) {
            std::vector<String> names;
            String next_marker;
            page++;
            log_memory_snapshot("HTTP list");
            if (!azure_blob_list_page(sas, prefix, marker, kBlobListMaxResults, names, next_marker, kBlobHttpTimeoutMs, kBlobHttpRetries, kBlobHttpRetryDelayMs)) {
                LOGW("Blob", "List failed (prefix=%s page %lu)", prefix.c_str(), (unsigned long)page);
                break;
            }

            // Filter to .g4 and sort lexicographically.
            std::vector<String> filtered;
            filtered.reserve(names.size());
            for (const auto &name : names) {
                if (name_is_g4(name)) {
                    filtered.push_back(name);
                }
            }

            std::sort(filtered.begin(), filtered.end(), [](const String &a, const String &b) {
                return a.compareTo(b) < 0;
            });

            for (const auto &name : filtered) {
                if (name.length() > kMaxG4NameLen) {
                    LOGW("Blob", "Skip long blob name: %s", name.c_str());
                    continue;
                }

                const String path = make_sd_path(name);
                if (path.length() == 0) {
                    LOGW("Blob", "Skip invalid blob name");
                    continue;
                }

                LOGI("Blob", "Attempting %s", name.c_str());
                uint8_t *buffer = nullptr;
                size_t size = 0;
                if (!download_blob_to_buffer(sas, name, &buffer, &size)) {
                    LOGW("Blob", "Download failed: %s", name.c_str());
                    continue;
                }

                if (!enqueue_sd_upload_and_wait(name, buffer, size)) {
                    LOGW("Blob", "Upload failed: %s", name.c_str());
                    continue;
                }

                LOGI("Blob", "Stored on SD: %s", path.c_str());
                rtc_image_state_set_priority_image_name(name.c_str());

                if (!delete_blob(sas, name)) {
                    LOGW("Blob", "Delete failed (will retry next wake): %s", name.c_str());
                }
                return true;
            }

            if (next_marker.length() == 0) break;
            marker = next_marker;
        }
    }

    return false;
}
