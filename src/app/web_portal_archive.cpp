#include "web_portal_archive.h"

#include "azure_blob_client.h"
#include "config_manager.h"
#include "log_manager.h"
#include "web_portal.h"
#include "web_portal_auth.h"
#include "web_portal_cors.h"

#include <esp_heap_caps.h>

namespace {
static constexpr size_t kMaxG4NameLen = 127;

// Aggressive UI timeouts: thumbnails are non-critical.
static constexpr uint32_t kPreviewTimeoutMs = 2500;
static constexpr uint8_t kPreviewRetries = 2;
static constexpr uint32_t kPreviewRetryDelayMs = 100;

static bool is_valid_preview_name(const String &name) {
    if (name.length() == 0 || name.length() > kMaxG4NameLen) return false;
    if (name.indexOf('\\') >= 0) return false;
    if (name.indexOf("..") >= 0) return false;

    String lower = name;
    lower.toLowerCase();
    if (!lower.endsWith(".g4")) return false;

    const int slash_pos = name.indexOf('/');
    if (slash_pos < 0) return false;
    if (name.lastIndexOf('/') != slash_pos) return false;
    return name.startsWith("queue-permanent/") || name.startsWith("queue-temporary/");
}

static String derive_thumb_blob_name(const String &g4_name) {
    // queue-permanent/<x>.g4 -> all/permanent/<x>__thumb.jpg
    // queue-temporary/<x>.g4 -> all/temporary/<x>__thumb.jpg
    if (g4_name.length() < 3) return String();
    const String base = g4_name.substring(0, g4_name.length() - 3);
    if (base.startsWith("queue-permanent/")) {
        return String("all/permanent/") + base.substring(strlen("queue-permanent/")) + "__thumb.jpg";
    }
    if (base.startsWith("queue-temporary/")) {
        return String("all/temporary/") + base.substring(strlen("queue-temporary/")) + "__thumb.jpg";
    }
    return String();
}

} // namespace

void handleGetArchivePreview(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;

    const auto send_no_store = [&](int code, const __FlashStringHelper *type, const String &body) {
        AsyncWebServerResponse *resp = request->beginResponse(code, type, body);
        resp->addHeader("Cache-Control", "no-store");
        web_portal_add_cors_headers(resp);
        request->send(resp);
    };

    // Not supported in AP/captive portal mode.
    if (web_portal_is_ap_mode()) {
        send_no_store(404, F("text/plain"), "Not available");
        return;
    }

    if (!request->hasParam("name") || !request->hasParam("kind")) {
        send_no_store(400, F("text/plain"), "Missing name or kind");
        return;
    }

    const String name = request->getParam("name")->value();
    const String kind = request->getParam("kind")->value();

    if (kind != "thumb") {
        send_no_store(400, F("text/plain"), "Invalid kind");
        return;
    }

    if (!is_valid_preview_name(name)) {
        send_no_store(400, F("text/plain"), "Invalid name");
        return;
    }

    DeviceConfig *config = web_portal_get_current_config();
    if (!config || strlen(config->blob_sas_url) == 0) {
        send_no_store(503, F("text/plain"), "Blob SAS not configured");
        return;
    }

    AzureSasUrlParts sas;
    if (!azure_blob_parse_sas_url(config->blob_sas_url, sas)) {
        send_no_store(503, F("text/plain"), "Invalid blob SAS URL");
        return;
    }

    const String blob_name = derive_thumb_blob_name(name);
    if (blob_name.length() == 0) {
        send_no_store(400, F("text/plain"), "Invalid name");
        return;
    }

    uint8_t *buf = nullptr;
    size_t size = 0;
    int http_code = 0;

    const bool ok = azure_blob_download_to_buffer_ex(
        sas,
        blob_name,
        &buf,
        &size,
        kPreviewTimeoutMs,
        kPreviewRetries,
        kPreviewRetryDelayMs,
        &http_code
    );

    if (!ok || !buf || size == 0) {
        if (buf) {
            heap_caps_free(buf);
            buf = nullptr;
        }

        if (http_code == 404) {
            LOGI("API", "All thumb missing: %s", blob_name.c_str());
            send_no_store(404, F("text/plain"), "Not found");
            return;
        }

        LOGW("API", "All thumb fetch failed http=%d name=%s blob=%s", http_code, name.c_str(), blob_name.c_str());

        send_no_store(502, F("text/plain"), "Upstream fetch failed");
        return;
    }

    // Serve without copying the whole payload into a String.
    AsyncWebServerResponse *response = request->beginChunkedResponse(
        "image/jpeg",
        [buf, size](uint8_t *out, size_t maxLen, size_t index) mutable -> size_t {
            if (index >= size) {
                if (buf) {
                    heap_caps_free(buf);
                    buf = nullptr;
                }
                return 0;
            }
            const size_t remaining = size - index;
            const size_t to_copy = (remaining < maxLen) ? remaining : maxLen;
            memcpy(out, buf + index, to_copy);
            return to_copy;
        }
    );

    response->addHeader("Cache-Control", "public, max-age=300");
    web_portal_add_cors_headers(response);
    request->send(response);
}
