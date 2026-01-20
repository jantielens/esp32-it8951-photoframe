#include "blob_pull.h"

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
static constexpr uint8_t kBlobListMaxResults = 50;
static constexpr const char *kAzureMsVersion = "2020-10-02";
static constexpr size_t kMaxG4NameLen = 63;
static constexpr uint32_t kBlobUploadJobTimeoutMs = 120000;

struct SasUrlParts {
    String base;
    String query;
    bool https;
};

static bool parse_sas_url(const char *url, SasUrlParts &out) {
    if (!url || strlen(url) == 0) return false;
    String raw(url);
    raw.trim();
    if (raw.length() == 0) return false;

    const int q = raw.indexOf('?');
    if (q < 0) return false;

    out.base = raw.substring(0, q);
    out.query = raw.substring(q + 1);
    out.base.trim();
    out.query.trim();
    if (out.base.length() == 0 || out.query.length() == 0) return false;

    out.https = out.base.startsWith("https://");
    return true;
}

static String get_query_param(const String &query, const char *key) {
    if (!key || !key[0]) return String();
    const String needle = String(key) + "=";
    int start = query.indexOf(needle);
    if (start < 0) return String();
    start += needle.length();
    int end = query.indexOf('&', start);
    if (end < 0) end = query.length();
    if (end <= start) return String();
    return query.substring(start, end);
}

static bool is_unreserved(char c) {
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

static String url_encode(const String &input) {
    String out;
    out.reserve(input.length() * 2);
    for (size_t i = 0; i < input.length(); i++) {
        const char c = input[i];
        if (is_unreserved(c)) {
            out += c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
            out += buf;
        }
    }
    return out;
}

static bool name_is_g4(const String &name) {
    if (name.length() < 3) return false;
    String lower = name;
    lower.toLowerCase();
    return lower.endsWith(".g4");
}

static String build_list_url(const SasUrlParts &sas, const String &marker) {
    String url = sas.base;
    url += "?";
    url += sas.query;
    url += "&restype=container&comp=list&maxresults=";
    url += String(kBlobListMaxResults);
    if (marker.length() > 0) {
        url += "&marker=";
        url += url_encode(marker);
    }
    return url;
}

static String build_blob_url(const SasUrlParts &sas, const String &name) {
    String url = sas.base;
    if (!url.endsWith("/")) {
        url += "/";
    }
    url += url_encode(name);
    url += "?";
    url += sas.query;
    return url;
}

static bool http_begin(HTTPClient &http, WiFiClient &plain, WiFiClientSecure &tls, const SasUrlParts &sas, const String &url) {
    http.setTimeout(kBlobHttpTimeoutMs);
    if (sas.https) {
        tls.setInsecure();
        return http.begin(tls, url);
    }
    return http.begin(plain, url);
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

static bool http_get_string_with_retry(const SasUrlParts &sas, const String &url, String &out_body) {
    for (uint8_t attempt = 1; attempt <= kBlobHttpRetries; attempt++) {
        HTTPClient http;
        WiFiClient plain;
        WiFiClientSecure tls;
        if (!http_begin(http, plain, tls, sas, url)) {
            LOGW("Blob", "HTTP begin failed (attempt %u/%u)", attempt, kBlobHttpRetries);
        } else {
            http.addHeader("x-ms-version", kAzureMsVersion);
            log_memory_snapshot("HTTP list");
            const int code = http.GET();
            if (code == HTTP_CODE_OK) {
                out_body = http.getString();
                http.end();
                return true;
            }
            String err_body = http.getString();
            if (err_body.length() > 256) {
                err_body = err_body.substring(0, 256);
            }
            LOGW("Blob", "HTTP GET failed (%d) attempt %u/%u body=%s", code, attempt, kBlobHttpRetries, err_body.c_str());
        }
        http.end();
        delay(kBlobHttpRetryDelayMs * attempt);
    }
    return false;
}

static void parse_list_xml(const String &xml, std::vector<String> &names, String &next_marker) {
    names.clear();
    next_marker = "";

    size_t pos = 0;
    while (true) {
        const int start = xml.indexOf("<Name>", pos);
        if (start < 0) break;
        const int end = xml.indexOf("</Name>", start);
        if (end < 0) break;
        const int content_start = start + 6;
        if (end > content_start) {
            String name = xml.substring(content_start, end);
            names.push_back(name);
        }
        pos = static_cast<size_t>(end + 7);
    }

    const int marker_start = xml.indexOf("<NextMarker>");
    if (marker_start >= 0) {
        const int marker_end = xml.indexOf("</NextMarker>", marker_start);
        if (marker_end >= 0) {
            const int content_start = marker_start + 12;
            if (marker_end > content_start) {
                next_marker = xml.substring(content_start, marker_end);
            }
        }
    }
}

static bool list_blob_page(const SasUrlParts &sas, const String &marker, std::vector<String> &out_names, String &out_next_marker) {
    const String url = build_list_url(sas, marker);
    String body;
    if (!http_get_string_with_retry(sas, url, body)) {
        LOGW("Blob", "List request failed");
        return false;
    }

    parse_list_xml(body, out_names, out_next_marker);
    if (out_names.empty()) {
        LOGI("Blob", "List returned no blobs");
        return true;
    }

    // Filter to .g4 and sort lexicographically.
    std::vector<String> filtered;
    filtered.reserve(out_names.size());
    for (const auto &name : out_names) {
        if (name_is_g4(name)) {
            filtered.push_back(name);
        }
    }

    std::sort(filtered.begin(), filtered.end(), [](const String &a, const String &b) {
        return a.compareTo(b) < 0;
    });

    out_names.swap(filtered);
    return true;
}

static String make_sd_path(const String &name) {
    if (name.length() == 0) return String();
    String path = "/";
    path += name;
    return path;
}

static bool download_blob_to_buffer(const SasUrlParts &sas, const String &name, uint8_t **out_buf, size_t *out_size) {
    const String url = build_blob_url(sas, name);

    if (out_buf) *out_buf = nullptr;
    if (out_size) *out_size = 0;

    for (uint8_t attempt = 1; attempt <= kBlobHttpRetries; attempt++) {
        HTTPClient http;
        WiFiClient plain;
        WiFiClientSecure tls;
        if (!http_begin(http, plain, tls, sas, url)) {
            LOGW("Blob", "Download begin failed (attempt %u/%u)", attempt, kBlobHttpRetries);
        } else {
            http.addHeader("x-ms-version", kAzureMsVersion);
            log_memory_snapshot("HTTP download");
            const int code = http.GET();
            if (code == HTTP_CODE_OK) {
                WiFiClient *stream = http.getStreamPtr();
                int remaining = http.getSize();
                if (remaining <= 0) {
                    http.end();
                    LOGW("Blob", "Missing content-length for %s", name.c_str());
                    return false;
                }

                const size_t total_size = static_cast<size_t>(remaining);
                uint8_t *buffer = (uint8_t *)heap_caps_malloc(total_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!buffer) {
                    buffer = (uint8_t *)heap_caps_malloc(total_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                }
                if (!buffer) {
                    http.end();
                    LOGE("Blob", "PSRAM alloc failed (%lu bytes)", (unsigned long)total_size);
                    return false;
                }

                uint8_t buf[1024];
                size_t total = 0;
                bool ok = true;

                while (http.connected() && (remaining > 0 || remaining == -1)) {
                    const size_t available = stream->available();
                    if (available) {
                        const size_t to_read = available > sizeof(buf) ? sizeof(buf) : available;
                        const int read = stream->readBytes(buf, to_read);
                        if (read <= 0) break;
                        if (total + static_cast<size_t>(read) > total_size) {
                            ok = false;
                            break;
                        }
                        memcpy(buffer + total, buf, static_cast<size_t>(read));
                        total += static_cast<size_t>(read);
                        if (remaining > 0) remaining -= read;
                    } else {
                        delay(1);
                    }
                }
                http.end();

                if (!ok || (remaining > 0) || total != total_size) {
                    LOGW("Blob", "Download incomplete (%lu bytes)", (unsigned long)total);
                    heap_caps_free(buffer);
                    return false;
                }

                if (out_buf) *out_buf = buffer;
                if (out_size) *out_size = total_size;
                LOGI("Blob", "Downloaded %s (%lu bytes)", name.c_str(), (unsigned long)total_size);
                return true;
            }

            LOGW("Blob", "Download failed (%d) attempt %u/%u", code, attempt, kBlobHttpRetries);
        }
        http.end();
        delay(kBlobHttpRetryDelayMs * attempt);
    }

    return false;
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

static bool delete_blob(const SasUrlParts &sas, const String &name) {
    const String url = build_blob_url(sas, name);

    for (uint8_t attempt = 1; attempt <= kBlobHttpRetries; attempt++) {
        HTTPClient http;
        WiFiClient plain;
        WiFiClientSecure tls;
        if (!http_begin(http, plain, tls, sas, url)) {
            LOGW("Blob", "Delete begin failed (attempt %u/%u)", attempt, kBlobHttpRetries);
        } else {
            http.addHeader("x-ms-version", kAzureMsVersion);
            const int code = http.sendRequest("DELETE");
            if (code == HTTP_CODE_ACCEPTED || code == HTTP_CODE_NO_CONTENT || code == HTTP_CODE_OK) {
                http.end();
                return true;
            }
            LOGW("Blob", "Delete failed (%d) attempt %u/%u", code, attempt, kBlobHttpRetries);
        }
        http.end();
        delay(kBlobHttpRetryDelayMs * attempt);
    }

    return false;
}

}

bool blob_pull_download_once(const DeviceConfig &config, SPIClass &spi, const SdCardPins &pins, uint32_t frequency_hz) {
    if (strlen(config.blob_sas_url) == 0) return false;

    SasUrlParts sas = {};
    if (!parse_sas_url(config.blob_sas_url, sas)) {
        LOGE("Blob", "Invalid SAS URL (expected https://...?...)");
        return false;
    }

    const String sp = get_query_param(sas.query, "sp");
    const String sr = get_query_param(sas.query, "sr");
    const String spr = get_query_param(sas.query, "spr");
    const String sv = get_query_param(sas.query, "sv");
    const String se = get_query_param(sas.query, "se");
    LOGI("Blob", "SAS params sp=%s sr=%s spr=%s sv=%s se=%s",
         sp.length() ? sp.c_str() : "(none)",
         sr.length() ? sr.c_str() : "(none)",
         spr.length() ? spr.c_str() : "(none)",
         sv.length() ? sv.c_str() : "(none)",
         se.length() ? se.c_str() : "(none)");

    LOGI("Blob", "Pull-on-wake: starting");

    if (WiFi.status() != WL_CONNECTED) {
        LOGW("Blob", "WiFi not connected; skipping blob pull");
        return false;
    }

    if (!sd_storage_configure(spi, pins, frequency_hz)) {
        LOGE("Blob", "SD service init failed; skipping blob pull");
        return false;
    }

    bool downloaded = false;
    String marker;
    uint32_t page = 0;

    while (!downloaded) {
        std::vector<String> names;
        String next_marker;
        page++;

        if (!list_blob_page(sas, marker, names, next_marker)) {
            LOGW("Blob", "List failed (page %lu)", (unsigned long)page);
            break;
        }

        if (names.empty()) {
            LOGI("Blob", "No .g4 blobs found (page %lu)", (unsigned long)page);
        }

        for (const auto &name : names) {
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
            downloaded = true;

            if (!delete_blob(sas, name)) {
                LOGW("Blob", "Delete failed (will retry next wake): %s", name.c_str());
            }
            break;
        }

        if (downloaded) break;
        if (next_marker.length() == 0) break;
        marker = next_marker;
    }
    return downloaded;
}
