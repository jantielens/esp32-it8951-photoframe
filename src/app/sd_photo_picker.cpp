#include "sd_photo_picker.h"

#include "log_manager.h"

#include <ctype.h>

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
