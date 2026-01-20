#include "image_render_service.h"

#include "it8951_renderer.h"
#include "log_manager.h"
#include "rtc_state.h"

#include <SD.h>

namespace {
static bool render_g4_path(const String &path) {
    const unsigned long disp_start = millis();
    if (!it8951_renderer_init()) {
        LOGE("EINK", "Init failed");
        return false;
    }
    LOG_DURATION("EINK", "Init", disp_start);

    LOGI("EINK", "Render G4=%s", path.c_str());
    if (!it8951_render_g4(path.c_str())) {
        LOGE("EINK", "Render G4 failed");
        return false;
    }
    LOGI("EINK", "Render G4 complete");
    return true;
}
}

bool image_render_service_render_next(SdImageSelectMode mode, uint32_t last_index, const char *last_name) {
    const char *priority_name = rtc_image_state_get_priority_image_name();
    if (priority_name && priority_name[0] != '\0') {
        const String priority_path = "/" + String(priority_name);
        rtc_image_state_clear_priority_image_name();
        if (SD.exists(priority_path)) {
            if (!render_g4_path(priority_path)) {
                return false;
            }
            if (mode == SdImageSelectMode::Sequential) {
                rtc_image_state_set_last_image_name(priority_name);
            }
            return true;
        }
    }

    char g4_path[64];
    uint32_t selected_index = 0;
    char selected_name[64] = {0};

    if (!sd_pick_g4_image(g4_path, sizeof(g4_path), mode, last_index, last_name, &selected_index, selected_name, sizeof(selected_name))) {
        LOGW("SD", "No .g4 files found");
        return false;
    }

    if (!render_g4_path(String(g4_path))) {
        return false;
    }

    if (mode == SdImageSelectMode::Sequential) {
        rtc_image_state_set_last_image_index(selected_index);
        rtc_image_state_set_last_image_name(selected_name);
    }

    return true;
}
