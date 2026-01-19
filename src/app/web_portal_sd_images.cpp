#include "web_portal_sd_images.h"

#include "web_portal_auth.h"
#include "web_portal_json.h"
#include "web_portal.h"
#include "log_manager.h"
#include "rtc_state.h"
#include "it8951_renderer.h"

#include <SD.h>
#include <vector>
#include <algorithm>

namespace {
static constexpr size_t kMaxG4UploadBytes = 2 * 1024 * 1024;
static constexpr size_t kMaxG4NameLen = 63;

struct UploadState {
    File file;
    size_t size = 0;
    bool error = false;
    bool responded = false;
    char name[kMaxG4NameLen + 1] = {0};
};

static bool is_valid_g4_name(const String &name) {
    if (name.length() == 0 || name.length() > kMaxG4NameLen) return false;
    if (name.indexOf('/') >= 0 || name.indexOf('\\') >= 0) return false;
    if (!name.endsWith(".g4")) return false;
    return true;
}

static bool collect_g4_names(std::vector<String> &names) {
    File root = SD.open("/");
    if (!root) return false;
    if (!root.isDirectory()) {
        root.close();
        return false;
    }

    File file = root.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            const char *name = file.name();
            if (name && is_valid_g4_name(String(name))) {
                names.push_back(String(name));
            }
        }
        file.close();
        file = root.openNextFile();
    }

    root.close();
    return true;
}

static void sort_names_kiss(std::vector<String> &names) {
    std::sort(names.begin(), names.end(), [](const String &a, const String &b) {
        return a.compareTo(b) < 0;
    });
}

static void send_upload_error(AsyncWebServerRequest *request, UploadState *state, int code, const char *msg) {
    if (!request || !state || state->responded) return;
    web_portal_send_json_error(request, code, msg);
    state->responded = true;
}

static bool should_use_sequential_mode() {
    DeviceConfig *config = web_portal_get_current_config();
    if (!config) return false;
    return strcmp(config->image_selection_mode, "sequential") == 0;
}

static bool try_set_sequential_index_for_name(const String &name) {
    std::vector<String> names;
    if (!collect_g4_names(names)) return false;
    if (names.empty()) return false;

    sort_names_kiss(names);
    for (size_t i = 0; i < names.size(); i++) {
        if (names[i] == name) {
            rtc_image_state_init();
            rtc_image_state_set_last_image_index(static_cast<uint32_t>(i));
            return true;
        }
    }
    return false;
}
}

void handleGetSdImages(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;

    std::vector<String> names;
    if (!collect_g4_names(names)) {
        LOGE("API", "GET /api/sd/images: SD unavailable");
        web_portal_send_json_error(request, 500, "SD unavailable");
        return;
    }

    sort_names_kiss(names);
    const size_t count = names.size();
    const size_t capacity = JSON_OBJECT_SIZE(3) + JSON_ARRAY_SIZE(count) + (count * 72) + 256;
    auto doc = make_psram_json_doc(capacity);
    if (!doc || doc->capacity() == 0) {
        web_portal_send_json_error(request, 503, "Out of memory");
        return;
    }

    (*doc)["success"] = true;
    (*doc)["count"] = static_cast<uint32_t>(count);
    JsonArray files = (*doc).createNestedArray("files");
    for (const auto &name : names) {
        files.add(name);
    }

    web_portal_send_json_chunked(request, doc, 200);
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

    const String path = "/" + name;
    if (!SD.exists(path)) {
        LOGW("API", "DELETE /api/sd/images: not found %s", name.c_str());
        web_portal_send_json_error(request, 404, "Not found");
        return;
    }

    if (!SD.remove(path)) {
        LOGE("API", "DELETE /api/sd/images: delete failed %s", name.c_str());
        web_portal_send_json_error(request, 500, "Delete failed");
        return;
    }

    request->send(200, "application/json", "{\"success\":true}");
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

    const String path = "/" + name;
    if (!SD.exists(path)) {
        LOGW("API", "POST /api/sd/images/display: not found %s", name.c_str());
        web_portal_send_json_error(request, 404, "Not found");
        return;
    }

    LOGI("API", "POST /api/sd/images/display: %s", name.c_str());

    if (!it8951_renderer_init()) {
        LOGE("API", "POST /api/sd/images/display: display init failed");
        web_portal_send_json_error(request, 500, "Display init failed");
        return;
    }

    if (!it8951_render_g4(path.c_str())) {
        LOGE("API", "POST /api/sd/images/display: render failed %s", name.c_str());
        web_portal_send_json_error(request, 500, "Render failed");
        return;
    }

    if (should_use_sequential_mode()) {
        if (!try_set_sequential_index_for_name(name)) {
            LOGW("API", "Failed to update sequential index for %s", name.c_str());
        }
    }

    request->send(200, "application/json", "{\"success\":true}");
}

void handleUploadSdImage(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
    if (!portal_auth_gate(request)) return;

    UploadState *state = reinterpret_cast<UploadState*>(request->_tempObject);
    if (index == 0) {
        if (!is_valid_g4_name(filename)) {
            LOGW("API", "POST /api/sd/images: invalid filename %s", filename.c_str());
            web_portal_send_json_error(request, 400, "Invalid filename");
            return;
        }

        state = new UploadState();
        request->_tempObject = state;
        strlcpy(state->name, filename.c_str(), sizeof(state->name));

        const String path = "/" + filename;
        if (SD.exists(path)) {
            SD.remove(path);
        }

        state->file = SD.open(path, FILE_WRITE);
        if (!state->file) {
            LOGE("API", "POST /api/sd/images: open failed %s", filename.c_str());
            send_upload_error(request, state, 500, "Open failed");
            state->error = true;
        }
    }

    if (!state) return;
    if (state->error) {
        if (final) {
            if (state->file) state->file.close();
            delete state;
            request->_tempObject = nullptr;
        }
        return;
    }

    if (state->size + len > kMaxG4UploadBytes) {
        LOGW("API", "POST /api/sd/images: file too large %s", state->name);
        state->error = true;
        if (state->file) state->file.close();
        const String path = "/" + String(state->name);
        SD.remove(path);
        send_upload_error(request, state, 413, "File too large");
    } else if (len) {
        const size_t written = state->file.write(data, len);
        state->size += written;
        if (written != len) {
            LOGE("API", "POST /api/sd/images: write failed %s", state->name);
            state->error = true;
            if (state->file) state->file.close();
            const String path = "/" + String(state->name);
            SD.remove(path);
            send_upload_error(request, state, 500, "Write failed");
        }
    }

    if (final) {
        if (state->file) state->file.close();
        if (!state->error && !state->responded) {
            request->send(200, "application/json", "{\"success\":true}");
        }
        delete state;
        request->_tempObject = nullptr;
    }
}
