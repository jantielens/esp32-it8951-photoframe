#include "sd_photo_picker.h"

#include "log_manager.h"

#include <ctype.h>
#include <vector>
#include <algorithm>

static constexpr size_t kMaxG4NameLen = 63;
static constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;
static constexpr uint32_t kNoG4LogIntervalMs = 60000;

static bool ends_with_bmp_case_insensitive(const char *name) {
    if (!name) return false;
    const size_t len = strlen(name);
    if (len < 4) return false;
    const char *ext = name + (len - 4);
    return (tolower(ext[0]) == '.') &&
           (tolower(ext[1]) == 'b') &&
           (tolower(ext[2]) == 'm') &&
           (tolower(ext[3]) == 'p');
}

bool sd_photo_picker_init(SPIClass &spi, const SdCardPins &pins, uint32_t frequency_hz) {
    const unsigned long start_ms = millis();

    if (pins.power >= 0) {
        pinMode(pins.power, OUTPUT);
        digitalWrite(pins.power, HIGH);
        delay(50);
    }

    spi.begin(pins.sck, pins.miso, pins.mosi, pins.cs);
    if (!SD.begin(pins.cs, spi, frequency_hz)) {
        LOG_DURATION("SD", "Begin", start_ms);
        return false;
    }
    LOG_DURATION("SD", "Begin", start_ms);
    return true;
}

bool sd_pick_random_bmp(char *out_path, size_t out_len) {
    if (!out_path || out_len == 0) return false;

    const unsigned long start_ms = millis();
    File root = SD.open("/");
    if (!root) return false;
    if (!root.isDirectory()) return false;

    bool found = false;
    uint32_t count = 0;

    File file = root.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            const char *name = file.name();
            if (ends_with_bmp_case_insensitive(name)) {
                count++;
                if (random(count) == 0) {
                    snprintf(out_path, out_len, "/%s", name);
                    found = true;
                }
            }
        }
        file.close();
        file = root.openNextFile();
    }

    LOGI("SD", "BMP count=%lu", (unsigned long)count);
    if (found) {
        LOGI("SD", "Pick path=%s", out_path);
    }
    LOG_DURATION("SD", "Scan", start_ms);

    return found;
}

static bool is_g4_file(const char *name) {
    if (!name) return false;
    const size_t len = strlen(name);
    if (len < 3) return false;
    return strcmp(name + (len - 3), ".g4") == 0;
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
            if (is_g4_file(name)) {
                const size_t len = strlen(name);
                if (len <= kMaxG4NameLen) {
                    names.push_back(String(name));
                } else {
                    LOGW("SD", "Skip long filename: %s", name);
                }
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

bool sd_pick_g4_image(
    char *out_path,
    size_t out_len,
    SdImageSelectMode mode,
    uint32_t last_index,
    const char *last_name,
    uint32_t *out_selected_index,
    char *out_selected_name,
    size_t out_selected_name_len
) {
    if (!out_path || out_len == 0) return false;

    std::vector<String> names;
    if (!collect_g4_names(names)) {
        LOGE("SD", "Failed to open SD root");
        return false;
    }

    const uint32_t count = static_cast<uint32_t>(names.size());
    if (count == 0) {
        static unsigned long last_no_g4_log_ms = 0;
        const unsigned long now = millis();
        if (now - last_no_g4_log_ms >= kNoG4LogIntervalMs) {
            LOGW("SD", "No .g4 images found");
            last_no_g4_log_ms = now;
        }
        return false;
    }

    sort_names_kiss(names);

    uint32_t index = 0;
    if (mode == SdImageSelectMode::Random) {
        index = static_cast<uint32_t>(random(count));
    } else {
        bool found_last = false;
        uint32_t base_index = kInvalidIndex;

        if (last_name && last_name[0] != '\0') {
            for (uint32_t i = 0; i < count; i++) {
                if (names[i] == last_name) {
                    base_index = i;
                    found_last = true;
                    break;
                }
            }
        }

        if (!found_last) {
            if (last_index != kInvalidIndex && last_index < count) {
                base_index = last_index;
            }
        }

        if (base_index == kInvalidIndex || base_index >= count) {
            index = 0;
        } else {
            index = (base_index + 1) % count;
        }
    }

    const String &name = names[index];
    if (name.length() + 1 >= out_len) return false;
    out_path[0] = '/';
    memcpy(out_path + 1, name.c_str(), name.length() + 1);

    if (out_selected_name && out_selected_name_len > 0) {
        strlcpy(out_selected_name, name.c_str(), out_selected_name_len);
    }

    if (out_selected_index) {
        *out_selected_index = index;
    }
    return true;
}
