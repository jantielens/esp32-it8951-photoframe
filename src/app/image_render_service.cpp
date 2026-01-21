#include "image_render_service.h"

#include "display_manager.h"
#include "it8951_renderer.h"
#include "log_manager.h"
#include "rtc_state.h"

#include <SD.h>
#include <time.h>
#include <algorithm>
#include <vector>
#include <cstdlib>

namespace {
static constexpr time_t kValidTimeThreshold = 1609459200; // 2021-01-01T00:00:00Z

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

static bool is_time_valid() {
    time_t now = time(nullptr);
    return now >= kValidTimeThreshold;
}

static time_t timegm_portable(struct tm *tm) {
    const char *tz = getenv("TZ");
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t t = mktime(tm);
    if (tz) {
        setenv("TZ", tz, 1);
    } else {
        unsetenv("TZ");
    }
    tzset();
    return t;
}

static bool parse_utc_timestamp(const char *ts, time_t *out_epoch) {
    if (!ts || !out_epoch) return false;
    // Expect YYYYMMDDTHHMMSSZ (16 chars)
    if (strlen(ts) != 16) return false;
    for (int i = 0; i < 16; i++) {
        const char c = ts[i];
        if (i == 8) {
            if (c != 'T') return false;
        } else if (i == 15) {
            if (c != 'Z') return false;
        } else if (c < '0' || c > '9') {
            return false;
        }
    }

    const int year = (ts[0] - '0') * 1000 + (ts[1] - '0') * 100 + (ts[2] - '0') * 10 + (ts[3] - '0');
    const int mon = (ts[4] - '0') * 10 + (ts[5] - '0');
    const int day = (ts[6] - '0') * 10 + (ts[7] - '0');
    const int hour = (ts[9] - '0') * 10 + (ts[10] - '0');
    const int min = (ts[11] - '0') * 10 + (ts[12] - '0');
    const int sec = (ts[13] - '0') * 10 + (ts[14] - '0');

    struct tm tm = {};
    tm.tm_year = year - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;
    tm.tm_isdst = 0;

    const time_t epoch = timegm_portable(&tm);
    if (epoch <= 0) return false;
    *out_epoch = epoch;
    return true;
}

static bool parse_temp_expiry(const String &name, time_t *out_epoch) {
    if (!out_epoch) return false;
    if (!name.startsWith("temp/")) return false;
    const int first_sep = name.indexOf("__", 5);
    if (first_sep < 0) return false;
    const String ts = name.substring(5, first_sep);
    return parse_utc_timestamp(ts.c_str(), out_epoch);
}

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
            const bool is_temp = String(priority_name).startsWith("temp/");
            if (is_temp) {
                rtc_image_state_set_last_temp_name(priority_name);
            } else {
                rtc_image_state_set_last_perm_name(priority_name);
            }
            rtc_image_state_set_last_was_temp(is_temp);
            return true;
        }
    }

    std::vector<String> perm_names;
    std::vector<String> temp_names;
    if (!list_g4_names_in_dir("/perm", "perm/", perm_names)) {
        LOGE("SD", "Failed to open /perm");
        return false;
    }
    if (!list_g4_names_in_dir("/temp", "temp/", temp_names)) {
        LOGE("SD", "Failed to open /temp");
        return false;
    }

    sort_names(perm_names);
    sort_names(temp_names);

    const bool can_cleanup = is_time_valid();
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
    if (selected_is_temp) {
        rtc_image_state_set_last_temp_name(selected_name.c_str());
    } else {
        rtc_image_state_set_last_perm_name(selected_name.c_str());
    }
    rtc_image_state_set_last_was_temp(selected_is_temp);
    return true;
}
