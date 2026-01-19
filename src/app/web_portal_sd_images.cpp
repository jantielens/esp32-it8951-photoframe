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

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
static constexpr size_t kMaxG4UploadBytes = 2 * 1024 * 1024;
static constexpr size_t kMaxG4NameLen = 63;

static portMUX_TYPE g_sd_mux = portMUX_INITIALIZER_UNLOCKED;
static bool g_sd_locked = false;

static portMUX_TYPE g_pending_mux = portMUX_INITIALIZER_UNLOCKED;
static bool g_pending_display = false;
static char g_pending_name[kMaxG4NameLen + 1] = {0};

struct UploadState {
    File file;
    size_t size = 0;
    bool error = false;
    bool responded = false;
    char name[kMaxG4NameLen + 1] = {0};
    bool locked = false;
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

static void release_upload_lock(UploadState *state) {
    if (!state || !state->locked) return;
    sd_images_unlock();
    state->locked = false;
}

static bool queue_display_name(const char *name) {
    if (!name) return false;
    portENTER_CRITICAL(&g_pending_mux);
    if (g_pending_display) {
        portEXIT_CRITICAL(&g_pending_mux);
        return false;
    }
    strlcpy(g_pending_name, name, sizeof(g_pending_name));
    g_pending_display = true;
    portEXIT_CRITICAL(&g_pending_mux);
    return true;
}

static void clear_pending_display() {
    portENTER_CRITICAL(&g_pending_mux);
    g_pending_display = false;
    g_pending_name[0] = '\0';
    portEXIT_CRITICAL(&g_pending_mux);
}

static bool peek_pending_display(char *out_name, size_t out_len) {
    if (!out_name || out_len == 0) return false;
    portENTER_CRITICAL(&g_pending_mux);
    const bool has_pending = g_pending_display;
    if (has_pending) {
        strlcpy(out_name, g_pending_name, out_len);
    }
    portEXIT_CRITICAL(&g_pending_mux);
    return has_pending;
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
            rtc_image_state_set_last_image_name(name.c_str());
            return true;
        }
    }
    return false;
}
}

void handleGetSdImages(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;

    if (!sd_images_try_lock("list")) {
        web_portal_send_json_error(request, 409, "SD busy");
        return;
    }

    std::vector<String> names;
    if (!collect_g4_names(names)) {
        LOGE("API", "GET /api/sd/images: SD unavailable");
        sd_images_unlock();
        web_portal_send_json_error(request, 500, "SD unavailable");
        return;
    }

    sort_names_kiss(names);
    const size_t count = names.size();
    const size_t capacity = JSON_OBJECT_SIZE(3) + JSON_ARRAY_SIZE(count) + (count * 72) + 256;
    auto doc = make_psram_json_doc(capacity);
    if (!doc || doc->capacity() == 0) {
        sd_images_unlock();
        web_portal_send_json_error(request, 503, "Out of memory");
        return;
    }

    (*doc)["success"] = true;
    (*doc)["count"] = static_cast<uint32_t>(count);
    JsonArray files = (*doc).createNestedArray("files");
    for (const auto &name : names) {
        files.add(name);
    }

    sd_images_unlock();
    web_portal_send_json_chunked(request, doc, 200);
}

void handleDeleteSdImage(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;

    if (!sd_images_try_lock("delete")) {
        web_portal_send_json_error(request, 409, "SD busy");
        return;
    }

    if (!request->hasParam("name")) {
        LOGW("API", "DELETE /api/sd/images: missing name");
        sd_images_unlock();
        web_portal_send_json_error(request, 400, "Missing name");
        return;
    }

    const String name = request->getParam("name")->value();
    if (!is_valid_g4_name(name)) {
        LOGW("API", "DELETE /api/sd/images: invalid name %s", name.c_str());
        sd_images_unlock();
        web_portal_send_json_error(request, 400, "Invalid name");
        return;
    }

    const String path = "/" + name;
    if (!SD.exists(path)) {
        LOGW("API", "DELETE /api/sd/images: not found %s", name.c_str());
        sd_images_unlock();
        web_portal_send_json_error(request, 404, "Not found");
        return;
    }

    if (!SD.remove(path)) {
        LOGE("API", "DELETE /api/sd/images: delete failed %s", name.c_str());
        sd_images_unlock();
        web_portal_send_json_error(request, 500, "Delete failed");
        return;
    }

    sd_images_unlock();
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

    if (!queue_display_name(name.c_str())) {
        web_portal_send_json_error(request, 409, "Display already pending");
        return;
    }

    bool checked = false;
    if (sd_images_try_lock("display_check")) {
        checked = true;
        const String path = "/" + name;
        if (!SD.exists(path)) {
            sd_images_unlock();
            clear_pending_display();
            LOGW("API", "POST /api/sd/images/display: not found %s", name.c_str());
            web_portal_send_json_error(request, 404, "Not found");
            return;
        }
        sd_images_unlock();
    }

    LOGI("API", "POST /api/sd/images/display: queued %s%s", name.c_str(), checked ? "" : " (unchecked)");
    request->send(202, "application/json", "{\"success\":true,\"queued\":true}");
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

        if (!sd_images_try_lock("upload")) {
            web_portal_send_json_error(request, 409, "SD busy");
            return;
        }

        state = new UploadState();
        request->_tempObject = state;
        strlcpy(state->name, filename.c_str(), sizeof(state->name));
        state->locked = true;

        const String path = "/" + filename;
        if (SD.exists(path)) {
            SD.remove(path);
        }

        state->file = SD.open(path, FILE_WRITE);
        if (!state->file) {
            LOGE("API", "POST /api/sd/images: open failed %s", filename.c_str());
            send_upload_error(request, state, 500, "Open failed");
            state->error = true;
            release_upload_lock(state);
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
        release_upload_lock(state);
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
            release_upload_lock(state);
        }
    }

    if (final) {
        if (state->file) state->file.close();
        if (!state->error && !state->responded) {
            request->send(200, "application/json", "{\"success\":true}");
        }
        release_upload_lock(state);
        delete state;
        request->_tempObject = nullptr;
    }
}

bool sd_images_try_lock(const char *reason) {
    (void)reason;
    portENTER_CRITICAL(&g_sd_mux);
    if (g_sd_locked) {
        portEXIT_CRITICAL(&g_sd_mux);
        return false;
    }
    g_sd_locked = true;
    portEXIT_CRITICAL(&g_sd_mux);
    return true;
}

void sd_images_unlock() {
    portENTER_CRITICAL(&g_sd_mux);
    g_sd_locked = false;
    portEXIT_CRITICAL(&g_sd_mux);
}

bool sd_images_is_busy() {
    portENTER_CRITICAL(&g_sd_mux);
    const bool locked = g_sd_locked;
    portEXIT_CRITICAL(&g_sd_mux);
    return locked;
}

bool sd_images_process_pending_display() {
    char name[kMaxG4NameLen + 1] = {0};
    if (!peek_pending_display(name, sizeof(name))) return false;

    if (!sd_images_try_lock("pending_display")) {
        return false;
    }

    // Re-check and consume pending request under lock.
    if (!peek_pending_display(name, sizeof(name))) {
        sd_images_unlock();
        return false;
    }
    clear_pending_display();

    const String path = "/" + String(name);
    if (!SD.exists(path)) {
        LOGW("API", "Pending display missing: %s", name);
        sd_images_unlock();
        return false;
    }

    if (!it8951_renderer_init()) {
        LOGE("API", "Pending display init failed");
        sd_images_unlock();
        return false;
    }

    LOGI("API", "Pending display render: %s", name);
    const bool ok = it8951_render_g4(path.c_str());
    if (!ok) {
        LOGE("API", "Pending display render failed %s", name);
    } else if (should_use_sequential_mode()) {
        if (!try_set_sequential_index_for_name(String(name))) {
            LOGW("API", "Failed to update sequential index for %s", name);
        }
    }

    sd_images_unlock();
    return ok;
}
