#include "image_render_service.h"

#include "display_manager.h"
#include "it8951_renderer.h"
#include "log_manager.h"
#include "rtc_state.h"
#include "time_utils.h"

#include <SD.h>
#include <algorithm>
#include <vector>

namespace {
static bool render_g4_path(const String &path) {
    const bool ui_was_active = display_manager_ui_is_active();
    if (ui_was_active) {
        display_manager_ui_stop();
    }

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

// Extract the expiry from queue-temporary/<EXPIRES_UTC>__<UPLOAD_UTC>__<slug>.g4.
// We only need the first timestamp for expiry cleanup.
static bool parse_temp_expiry(const String &name, time_t *out_epoch) {
    if (!out_epoch) return false;
    if (!name.startsWith("queue-temporary/")) return false;
    const int prefix_len = strlen("queue-temporary/");
    const int first_sep = name.indexOf("__", prefix_len);
    if (first_sep < 0) return false;
    const String ts = name.substring(prefix_len, first_sep);
    return time_utils::parse_utc_timestamp(ts.c_str(), out_epoch);
}

// Collect .g4 names from a single directory and apply a prefix so the caller
// receives logical paths like queue-permanent/<name> or queue-temporary/<name>.
static bool list_g4_names_in_dir(const char *dir, const char *prefix, std::vector<String> &out) {
    if (!dir) return false;
    if (!SD.exists(dir)) return true;

    File root = SD.open(dir);
    if (!root) return false;
    if (!root.isDirectory()) {
        root.close();
        return false;
    }

    File file = root.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            const char *name = file.name();
            if (name) {
                const size_t len = strlen(name);
                if (len >= 3 && strcmp(name + (len - 3), ".g4") == 0) {
                    String entry = prefix ? String(prefix) + String(name) : String(name);
                    out.push_back(entry);
                }
            }
        }
        file.close();
        file = root.openNextFile();
    }

    root.close();
    return true;
}

static void sort_names(std::vector<String> &names) {
    std::sort(names.begin(), names.end(), [](const String &a, const String &b) {
        return a.compareTo(b) < 0;
    });
}

static bool pick_random_from_list(const std::vector<String> &names, String &out) {
    if (names.empty()) return false;
    const size_t index = static_cast<size_t>(random(names.size()));
    out = names[index];
    return true;
}

static bool pick_sequential_from_list(const std::vector<String> &names, const char *last_name, String &out) {
    if (names.empty()) return false;
    size_t index = 0;
    bool found_last = false;
    if (last_name && last_name[0] != '\0') {
        for (size_t i = 0; i < names.size(); i++) {
            if (names[i] == last_name) {
                index = (i + 1) % names.size();
                found_last = true;
                break;
            }
        }
    }
    if (!found_last) {
        index = 0;
    }
    out = names[index];
    return true;
}

static bool select_from_list(
    const std::vector<String> &names,
    SdImageSelectMode mode,
    const char *last_name,
    String &out
) {
    if (mode == SdImageSelectMode::Random) {
        return pick_random_from_list(names, out);
    }
    return pick_sequential_from_list(names, last_name, out);
}

// Filter temp names, optionally deleting expired files if time is valid.
// When time is not valid, we skip cleanup and treat all temp files as candidates.
static bool build_temp_candidates(std::vector<String> &names, bool allow_cleanup, time_t now, std::vector<String> &out) {
    out.clear();
    for (const auto &name : names) {
        if (allow_cleanup) {
            time_t expiry = 0;
            if (parse_temp_expiry(name, &expiry) && expiry <= now) {
                const String path = "/" + name;
                if (SD.exists(path)) {
                    SD.remove(path);
                }
                continue;
            }
        }
        out.push_back(name);
    }
    return !out.empty();
}
}

bool image_render_service_render_next(SdImageSelectMode mode, uint32_t last_index, const char *last_name) {
    (void)last_index;
    (void)last_name;
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
            const bool is_temp = String(priority_name).startsWith("queue-temporary/");
            if (is_temp) {
                rtc_image_state_set_last_temp_name(priority_name);
            } else {
                rtc_image_state_set_last_perm_name(priority_name);
            }
            rtc_image_state_set_last_was_temp(is_temp);
            return true;
        }
    }

    // Build logical lists from /queue-permanent and /queue-temporary only (no root fallback).
    std::vector<String> perm_names;
    std::vector<String> temp_names;
    if (!list_g4_names_in_dir("/queue-permanent", "queue-permanent/", perm_names)) {
        LOGE("SD", "Failed to open /queue-permanent");
        return false;
    }
    if (!list_g4_names_in_dir("/queue-temporary", "queue-temporary/", temp_names)) {
        LOGE("SD", "Failed to open /queue-temporary");
        return false;
    }

    sort_names(perm_names);
    sort_names(temp_names);

    // Only delete expired temp files when we have a valid clock.
    const bool can_cleanup = time_utils::is_time_valid();
    time_t now = 0;
    if (can_cleanup) {
        now = time(nullptr);
    }

    std::vector<String> temp_candidates;
    const bool has_temp = build_temp_candidates(temp_names, can_cleanup, now, temp_candidates);
    const bool has_perm = !perm_names.empty();

    if (!has_temp && !has_perm) {
        LOGW("SD", "No .g4 files found");
        return false;
    }

    // Alternate permanent/temporary when both are available. If one is empty, always use the other.
    const bool last_was_temp = rtc_image_state_get_last_was_temp();
    bool choose_temp = false;
    if (has_temp && has_perm) {
        choose_temp = !last_was_temp;
    } else if (has_temp) {
        choose_temp = true;
    } else {
        choose_temp = false;
    }

    String selected_name;
    bool selected_is_temp = false;
    if (choose_temp && has_temp) {
        const char *last_temp = rtc_image_state_get_last_temp_name();
        if (!select_from_list(temp_candidates, mode, last_temp, selected_name)) {
            choose_temp = false;
        } else {
            selected_is_temp = true;
        }
    }

    if (!selected_is_temp) {
        const char *last_perm = rtc_image_state_get_last_perm_name();
        if (!select_from_list(perm_names, mode, last_perm, selected_name)) {
            if (has_temp) {
                const char *last_temp = rtc_image_state_get_last_temp_name();
                if (!select_from_list(temp_candidates, mode, last_temp, selected_name)) {
                    LOGW("SD", "No .g4 files found");
                    return false;
                }
                selected_is_temp = true;
            } else {
                LOGW("SD", "No .g4 files found");
                return false;
            }
        }
    }

    const String selected_path = "/" + selected_name;
    if (!render_g4_path(selected_path)) {
        return false;
    }

    if (mode == SdImageSelectMode::Sequential) {
        rtc_image_state_set_last_image_name(selected_name.c_str());
    }
    // Store per-queue last names so sequential mode advances within each queue.
    if (selected_is_temp) {
        rtc_image_state_set_last_temp_name(selected_name.c_str());
    } else {
        rtc_image_state_set_last_perm_name(selected_name.c_str());
    }
    rtc_image_state_set_last_was_temp(selected_is_temp);
    return true;
}
