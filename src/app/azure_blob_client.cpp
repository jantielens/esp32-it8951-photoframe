#include "azure_blob_client.h"

#include "log_manager.h"

#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>

namespace {
static constexpr const char *kAzureMsVersion = "2020-10-02";

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

static String build_list_url(const AzureSasUrlParts &sas, const String &prefix, const String &marker, uint16_t max_results) {
    String url = sas.base;
    url += "?";
    url += sas.query;
    url += "&restype=container&comp=list&maxresults=";
    url += String(max_results);

    // Prefix-scoped listing (required: avoid enumerating all/ unless explicitly needed).
    if (prefix.length() > 0) {
        url += "&prefix=";
        url += url_encode(prefix);
    }

    if (marker.length() > 0) {
        url += "&marker=";
        url += url_encode(marker);
    }
    return url;
}

static bool http_begin(HTTPClient &http, WiFiClient &plain, WiFiClientSecure &tls, const AzureSasUrlParts &sas, const String &url, uint32_t timeout_ms) {
    http.setTimeout(timeout_ms);
    if (sas.https) {
        tls.setInsecure();
        return http.begin(tls, url);
    }
    return http.begin(plain, url);
}

static bool http_get_string_with_retry(
    const AzureSasUrlParts &sas,
    const String &url,
    String &out_body,
    uint32_t timeout_ms,
    uint8_t retries,
    uint32_t retry_delay_ms
) {
    for (uint8_t attempt = 1; attempt <= retries; attempt++) {
        HTTPClient http;
        WiFiClient plain;
        WiFiClientSecure tls;
        if (!http_begin(http, plain, tls, sas, url, timeout_ms)) {
            LOGW("Azure", "HTTP begin failed (attempt %u/%u)", attempt, retries);
        } else {
            http.addHeader("x-ms-version", kAzureMsVersion);
            const int code = http.GET();
            if (code == HTTP_CODE_OK) {
                out_body = http.getString();
                http.end();
                return true;
            }
            String err_body = http.getString();
            if (err_body.length() > 256) err_body = err_body.substring(0, 256);
            LOGW("Azure", "HTTP GET failed (%d) attempt %u/%u body=%s", code, attempt, retries, err_body.c_str());
        }
        http.end();
        delay(retry_delay_ms * attempt);
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
            names.push_back(xml.substring(content_start, end));
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

} // namespace

bool azure_blob_parse_sas_url(const char *url, AzureSasUrlParts &out) {
    out.base = "";
    out.query = "";
    out.https = true;

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

    // Light logging to help diagnose SAS issues.
    const String sp = get_query_param(out.query, "sp");
    const String sr = get_query_param(out.query, "sr");
    const String spr = get_query_param(out.query, "spr");
    const String sv = get_query_param(out.query, "sv");
    const String se = get_query_param(out.query, "se");
    LOGI("Azure", "SAS params sp=%s sr=%s spr=%s sv=%s se=%s",
         sp.length() ? sp.c_str() : "(none)",
         sr.length() ? sr.c_str() : "(none)",
         spr.length() ? spr.c_str() : "(none)",
         sv.length() ? sv.c_str() : "(none)",
         se.length() ? se.c_str() : "(none)");

    return true;
}

String azure_blob_build_blob_url(const AzureSasUrlParts &sas, const String &blob_name) {
    String url = sas.base;
    if (!url.endsWith("/")) url += "/";

    // Encode blob path but keep '/' separators.
    String encoded;
    encoded.reserve(blob_name.length() * 2);
    for (size_t i = 0; i < blob_name.length(); i++) {
        const char c = blob_name[i];
        if (c == '/') {
            encoded += c;
        } else if (is_unreserved(c)) {
            encoded += c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
            encoded += buf;
        }
    }

    url += encoded;
    url += "?";
    url += sas.query;
    return url;
}

bool azure_blob_list_page(
    const AzureSasUrlParts &sas,
    const String &prefix,
    const String &marker,
    uint16_t max_results,
    std::vector<String> &out_names,
    String &out_next_marker,
    uint32_t timeout_ms,
    uint8_t retries,
    uint32_t retry_delay_ms
) {
    const String url = build_list_url(sas, prefix, marker, max_results);
    String body;
    if (!http_get_string_with_retry(sas, url, body, timeout_ms, retries, retry_delay_ms)) {
        LOGW("Azure", "List request failed (prefix=%s)", prefix.c_str());
        return false;
    }

    parse_list_xml(body, out_names, out_next_marker);
    return true;
}

bool azure_blob_download_to_buffer(
    const AzureSasUrlParts &sas,
    const String &blob_name,
    uint8_t **out_buf,
    size_t *out_size,
    uint32_t timeout_ms,
    uint8_t retries,
    uint32_t retry_delay_ms
) {
    return azure_blob_download_to_buffer_ex(
        sas,
        blob_name,
        out_buf,
        out_size,
        timeout_ms,
        retries,
        retry_delay_ms,
        nullptr
    );
}

bool azure_blob_download_to_buffer_ex(
    const AzureSasUrlParts &sas,
    const String &blob_name,
    uint8_t **out_buf,
    size_t *out_size,
    uint32_t timeout_ms,
    uint8_t retries,
    uint32_t retry_delay_ms,
    int *out_http_code
) {
    if (out_buf) *out_buf = nullptr;
    if (out_size) *out_size = 0;
    if (out_http_code) *out_http_code = 0;

    const String url = azure_blob_build_blob_url(sas, blob_name);

    for (uint8_t attempt = 1; attempt <= retries; attempt++) {
        HTTPClient http;
        WiFiClient plain;
        WiFiClientSecure tls;
        if (!http_begin(http, plain, tls, sas, url, timeout_ms)) {
            if (out_http_code) *out_http_code = 0;
            LOGW("Azure", "Download begin failed (attempt %u/%u)", attempt, retries);
        } else {
            http.addHeader("x-ms-version", kAzureMsVersion);
            const int code = http.GET();
            if (out_http_code) *out_http_code = code;
            if (code == HTTP_CODE_OK) {
                WiFiClient *stream = http.getStreamPtr();
                int remaining = http.getSize();
                if (remaining <= 0) {
                    http.end();
                    LOGW("Azure", "Missing content-length for %s", blob_name.c_str());
                    return false;
                }

                const size_t total_size = static_cast<size_t>(remaining);
                uint8_t *buffer = (uint8_t *)heap_caps_malloc(total_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!buffer) {
                    buffer = (uint8_t *)heap_caps_malloc(total_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                }
                if (!buffer) {
                    http.end();
                    LOGE("Azure", "Alloc failed (%lu bytes)", (unsigned long)total_size);
                    return false;
                }

                uint8_t buf[1024];
                size_t total = 0;
                bool ok = true;

                while (http.connected() && remaining > 0) {
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
                        remaining -= read;
                    } else {
                        delay(1);
                    }
                }

                http.end();

                if (!ok || remaining != 0 || total != total_size) {
                    LOGW("Azure", "Download incomplete (%lu/%lu)", (unsigned long)total, (unsigned long)total_size);
                    heap_caps_free(buffer);
                    return false;
                }

                if (out_buf) *out_buf = buffer;
                if (out_size) *out_size = total_size;
                return true;
            }

            LOGW("Azure", "Download failed (%d) attempt %u/%u", code, attempt, retries);
        }
        http.end();
        delay(retry_delay_ms * attempt);
    }

    return false;
}

bool azure_blob_delete(
    const AzureSasUrlParts &sas,
    const String &blob_name,
    uint32_t timeout_ms,
    uint8_t retries,
    uint32_t retry_delay_ms
) {
    const String url = azure_blob_build_blob_url(sas, blob_name);

    for (uint8_t attempt = 1; attempt <= retries; attempt++) {
        HTTPClient http;
        WiFiClient plain;
        WiFiClientSecure tls;
        if (!http_begin(http, plain, tls, sas, url, timeout_ms)) {
            LOGW("Azure", "Delete begin failed (attempt %u/%u)", attempt, retries);
        } else {
            http.addHeader("x-ms-version", kAzureMsVersion);
            const int code = http.sendRequest("DELETE");
            if (code == HTTP_CODE_ACCEPTED || code == HTTP_CODE_NO_CONTENT || code == HTTP_CODE_OK) {
                http.end();
                return true;
            }
            LOGW("Azure", "Delete failed (%d) attempt %u/%u", code, attempt, retries);
        }
        http.end();
        delay(retry_delay_ms * attempt);
    }

    return false;
}
