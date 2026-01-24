#include "web_portal_sd_images.h"

#include "sd_storage_service.h"
#include "web_portal_auth.h"
#include "web_portal_json.h"
#include "web_portal.h"
#include "log_manager.h"
#include "time_utils.h"

#include <esp_heap_caps.h>

namespace {
static constexpr size_t kMaxG4UploadBytes = 2 * 1024 * 1024;
static constexpr size_t kMaxG4NameLen = 127;

struct UploadState {
    uint8_t *buffer = nullptr;
    size_t size = 0;
    size_t capacity = 0;
    bool error = false;
    bool responded = false;
    char name[kMaxG4NameLen + 1] = {0};
};

static bool is_valid_g4_name(const String &name) {
    if (name.length() == 0 || name.length() > kMaxG4NameLen) return false;
    if (name.indexOf('\\') >= 0) return false;
    if (!name.endsWith(".g4")) return false;
    if (name.indexOf("..") >= 0) return false;

    const int slash_pos = name.indexOf('/');
    if (slash_pos < 0) return true;
    if (name.lastIndexOf('/') != slash_pos) return false;
    return name.startsWith("queue-permanent/") || name.startsWith("queue-temporary/");
}

static bool is_valid_perm_upload_name(const String &name) {
    if (name.length() == 0 || name.length() > kMaxG4NameLen) return false;
    if (name.indexOf('\\') >= 0) return false;
    if (!name.endsWith(".g4")) return false;
    if (name.indexOf("..") >= 0) return false;

    const int slash_pos = name.indexOf('/');
    if (slash_pos < 0) return true;
    if (name.lastIndexOf('/') != slash_pos) return false;
    return name.startsWith("queue-permanent/");
}

static void send_upload_error(AsyncWebServerRequest *request, UploadState *state, int code, const char *msg) {
    if (!request || !state || state->responded) return;
    web_portal_send_json_error(request, code, msg);
    state->responded = true;
}

static void free_upload_state(UploadState *state) {
    if (!state) return;
    if (state->buffer) {
        heap_caps_free(state->buffer);
        state->buffer = nullptr;
    }
    delete state;
}

static void send_job_queued(AsyncWebServerRequest *request, uint32_t job_id) {
    if (!request) return;
    if (job_id == 0) {
        web_portal_send_json_error(request, 503, "Queue full");
        return;
    }
    String body = String("{\"success\":true,\"queued\":true,\"job_id\":") + String(job_id) + "}";
    LOGI("API", "SD job queued id=%lu", (unsigned long)job_id);
    request->send(202, "application/json", body);
}

static const char *job_state_str(SdJobState state) {
    switch (state) {
        case SdJobState::Queued: return "queued";
        case SdJobState::Running: return "running";
        case SdJobState::Done: return "done";
        case SdJobState::Error: return "error";
        default: return "unknown";
    }
}

static const char *job_type_str(SdJobType type) {
    switch (type) {
        case SdJobType::List: return "list";
        case SdJobType::Delete: return "delete";
        case SdJobType::Upload: return "upload";
        case SdJobType::Display: return "display";
        case SdJobType::RenderNext: return "render_next";
        case SdJobType::SyncFromAzure: return "sync";
        default: return "unknown";
    }
}
}

void handleGetSdImages(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;
    const uint32_t job_id = sd_storage_enqueue_list();
    LOGI("API", "GET /api/sd/images -> job %lu", (unsigned long)job_id);
    send_job_queued(request, job_id);
}

void handleDeleteSdImage(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;
    if (!request->hasParam("name")) {
        LOGW("API", "DELETE /api/sd/images: missing name");
        web_portal_send_json_error(request, 400, "Missing name");
        return;
    }

    const String name = request->getParam("name")->value();
    if (!is_valid_g4_name(name)) {
        LOGW("API", "DELETE /api/sd/images: invalid name %s", name.c_str());
        web_portal_send_json_error(request, 400, "Invalid name");
        return;
    }

    const uint32_t job_id = sd_storage_enqueue_delete(name.c_str());
    LOGI("API", "DELETE /api/sd/images -> job %lu name=%s", (unsigned long)job_id, name.c_str());
    send_job_queued(request, job_id);
}

void handleDisplaySdImage(AsyncWebServerRequest *request) {
    LOGI("API", "Display request received");
    if (!portal_auth_gate(request)) return;

    if (!request->hasParam("name")) {
        LOGW("API", "POST /api/sd/images/display: missing name");
        web_portal_send_json_error(request, 400, "Missing name");
        return;
    }

    const String name = request->getParam("name")->value();
    if (!is_valid_g4_name(name)) {
        LOGW("API", "POST /api/sd/images/display: invalid name %s", name.c_str());
        web_portal_send_json_error(request, 400, "Invalid name");
        return;
    }

    const uint32_t job_id = sd_storage_enqueue_display(name.c_str());
    LOGI("API", "POST /api/sd/images/display -> job %lu name=%s", (unsigned long)job_id, name.c_str());
    send_job_queued(request, job_id);
}

void handleUploadSdImage(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
    if (!portal_auth_gate(request)) return;

    UploadState *state = reinterpret_cast<UploadState*>(request->_tempObject);
    if (index == 0) {
        String target_name = filename;
        if (filename.indexOf('/') < 0) {
            target_name = String("queue-permanent/") + filename;
        }

        if (!is_valid_perm_upload_name(target_name)) {
            LOGW("API", "POST /api/sd/images: invalid filename %s", filename.c_str());
            web_portal_send_json_error(request, 400, "Invalid filename");
            return;
        }

        const size_t total = request->contentLength();
        if (total == 0 || total > kMaxG4UploadBytes) {
            LOGW("API", "POST /api/sd/images: file too large %s", filename.c_str());
            web_portal_send_json_error(request, 413, "File too large");
            return;
        }

        state = new UploadState();
        if (!state) {
            web_portal_send_json_error(request, 503, "Out of memory");
            return;
        }
        request->_tempObject = state;
        strlcpy(state->name, target_name.c_str(), sizeof(state->name));
        LOGI("API", "POST /api/sd/images: start %s bytes=%u", state->name, (unsigned)total);
        state->capacity = total;
        state->buffer = static_cast<uint8_t *>(heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!state->buffer) {
            state->buffer = static_cast<uint8_t *>(heap_caps_malloc(total, MALLOC_CAP_8BIT));
        }
        if (!state->buffer) {
            web_portal_send_json_error(request, 503, "Out of memory");
            free_upload_state(state);
            request->_tempObject = nullptr;
            return;
        }
    }

    if (!state) return;
    if (state->error) {
        if (final) {
            free_upload_state(state);
            request->_tempObject = nullptr;
        }
        return;
    }

    if (state->size + len > state->capacity) {
        LOGW("API", "POST /api/sd/images: payload overflow %s", state->name);
        state->error = true;
        send_upload_error(request, state, 413, "File too large");
        if (final) {
            free_upload_state(state);
            request->_tempObject = nullptr;
        }
        return;
    }

    if (len) {
        memcpy(state->buffer + state->size, data, len);
        state->size += len;
    }

    if (final) {
        uint32_t job_id = sd_storage_enqueue_upload(state->name, state->buffer, state->size);
        if (job_id == 0) {
            send_upload_error(request, state, 503, "Queue full");
            free_upload_state(state);
        } else {
            state->buffer = nullptr;
            state->capacity = 0;
            state->size = 0;
            LOGI("API", "POST /api/sd/images: queued job %lu name=%s", (unsigned long)job_id, state->name);
            send_job_queued(request, job_id);
            free_upload_state(state);
        }
        request->_tempObject = nullptr;
    }
}

void handleGetSdJobStatus(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;
    if (!request->hasParam("id")) {
        web_portal_send_json_error(request, 400, "Missing id");
        return;
    }

    const uint32_t id = static_cast<uint32_t>(request->getParam("id")->value().toInt());
    SdJobInfo info = {};
    if (!sd_storage_get_job(id, &info)) {
        web_portal_send_json_error(request, 404, "Not found");
        return;
    }

    std::vector<String> names;
    const bool has_names = sd_storage_get_job_names(id, names);

    const size_t count = names.size();
    const size_t capacity = JSON_OBJECT_SIZE(8) + JSON_ARRAY_SIZE(count) + (count * 72) + 256;
    auto doc = make_psram_json_doc(capacity);
    if (!doc || doc->capacity() == 0) {
        web_portal_send_json_error(request, 503, "Out of memory");
        return;
    }

    (*doc)["success"] = true;
    (*doc)["id"] = info.id;
    (*doc)["type"] = job_type_str(info.type);
    (*doc)["state"] = job_state_str(info.state);
    (*doc)["ok"] = info.success;
    (*doc)["bytes"] = static_cast<uint32_t>(info.bytes);
    if (info.message[0]) {
        (*doc)["message"] = info.message;
    }
    if (has_names) {
        JsonArray files = (*doc).createNestedArray("files");
        for (const auto &name : names) {
            files.add(name);
        }
    }

    web_portal_send_json_chunked(request, doc, 200);
}

void handlePostSdSync(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;
    if (web_portal_is_ap_mode()) {
        // Hide this endpoint in core/AP mode.
        web_portal_send_json_error(request, 404, "Not found");
        return;
    }

    if (!time_utils::is_time_valid()) {
        web_portal_send_json_error(request, 409, "Time not synced");
        return;
    }

    DeviceConfig *cfg = web_portal_get_current_config();
    if (!cfg || cfg->blob_sas_url[0] == '\0') {
        web_portal_send_json_error(request, 409, "Blob SAS URL not configured");
        return;
    }

    const uint32_t job_id = sd_storage_enqueue_sync_from_azure(cfg->blob_sas_url);
    LOGI("API", "POST /api/sd/sync -> job %lu", (unsigned long)job_id);
    send_job_queued(request, job_id);
}
