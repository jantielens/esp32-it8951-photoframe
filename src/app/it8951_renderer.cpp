#include "it8951_renderer.h"

#include "board_config.h"
#include "log_manager.h"

#include <SD.h>
#include <GxEPD2.h>
#include <it8951/GxEPD2_it78_1872x1404.h>

// ---------------------------------------------------------------------------
// Display wiring (from board overrides)
// ---------------------------------------------------------------------------
GxEPD2_it78_1872x1404 display(IT8951_CS_PIN, IT8951_DC_PIN, IT8951_RST_PIN, IT8951_BUSY_PIN);

static bool g_display_ready = false;

static const uint16_t kInputBufferPixels = 1872;
static const uint16_t kMaxRowWidth = 1872;
static const uint16_t kMaxPalettePixels = 256;

static uint8_t input_buffer[3 * kInputBufferPixels];
static uint8_t output_row_gray_buffer[kMaxRowWidth];
static uint8_t grey_palette_buffer[kMaxPalettePixels];

static uint8_t read8(File &f) {
    return f.read();
}

static uint16_t read16(File &f) {
    uint16_t result;
    ((uint8_t *)&result)[0] = f.read();
    ((uint8_t *)&result)[1] = f.read();
    return result;
}

static uint32_t read32(File &f) {
    uint32_t result;
    ((uint8_t *)&result)[0] = f.read();
    ((uint8_t *)&result)[1] = f.read();
    ((uint8_t *)&result)[2] = f.read();
    ((uint8_t *)&result)[3] = f.read();
    return result;
}

static bool draw_bmp_16gray(File &file, int16_t x, int16_t y) {
    const unsigned long start_ms = millis();
    if ((x >= display.WIDTH) || (y >= display.HEIGHT)) return false;

    bool valid = false;
    bool flip = false;

    uint16_t signature = read16(file);
    if (signature != 0x4D42) {
        Serial.println("BMP signature mismatch");
        return false;
    }

    uint32_t file_size = read32(file);
    (void)file_size;
    uint32_t creator_bytes = read32(file);
    (void)creator_bytes;
    uint32_t image_offset = read32(file);
    uint32_t header_size = read32(file);
    uint32_t width = read32(file);
    int32_t height = (int32_t)read32(file);
    uint16_t planes = read16(file);
    uint16_t depth = read16(file);
    uint32_t format = read32(file);

    if ((planes == 1) && ((format == 0) || (format == 3))) {
        uint32_t row_size = (width * depth / 8 + 3) & ~3;
        if (depth < 8) {
            row_size = ((width * depth + 8 - depth) / 8 + 3) & ~3;
        }
        if (height < 0) {
            height = -height;
            flip = false;
        }

        uint16_t w = width;
        uint16_t h = height;
        if ((x + w - 1) >= display.WIDTH) w = display.WIDTH - x;
        if ((y + h - 1) >= display.HEIGHT) h = display.HEIGHT - y;

        if (w <= kMaxRowWidth) {
            valid = true;
            uint8_t bitmask = 0xFF;
            uint8_t bitshift = 8 - depth;
            uint16_t red, green, blue;
            uint8_t grey;

            if (depth <= 8) {
                if (depth < 8) bitmask >>= depth;
                file.seek(image_offset - (4 << depth));
                for (uint16_t pn = 0; pn < (1 << depth); pn++) {
                    blue = read8(file);
                    green = read8(file);
                    red = read8(file);
                    read8(file);
                    grey = uint8_t((red + green + blue) / 3);
                    grey_palette_buffer[pn] = grey;
                }
            }

            display.clearScreen();

            uint32_t row_position = flip ? image_offset + (height - h) * row_size : image_offset;
            const unsigned long rows_start = millis();
            for (uint16_t row = 0; row < h; row++, row_position += row_size) {
                uint32_t in_remain = row_size;
                uint32_t in_idx = 0;
                uint32_t in_bytes = 0;
                uint8_t in_byte = 0;
                uint8_t in_bits = 0;
                uint32_t out_idx = 0;

                file.seek(row_position);
                for (uint16_t col = 0; col < w; col++) {
                    if (in_idx >= in_bytes) {
                        in_bytes = file.read(input_buffer, in_remain > sizeof(input_buffer) ? sizeof(input_buffer) : in_remain);
                        in_remain -= in_bytes;
                        in_idx = 0;
                    }

                    switch (depth) {
                        case 32:
                            blue = input_buffer[in_idx++];
                            green = input_buffer[in_idx++];
                            red = input_buffer[in_idx++];
                            in_idx++;
                            grey = uint8_t((red + green + blue) / 3);
                            break;
                        case 24:
                            blue = input_buffer[in_idx++];
                            green = input_buffer[in_idx++];
                            red = input_buffer[in_idx++];
                            grey = uint8_t((red + green + blue) / 3);
                            break;
                        case 16: {
                            uint8_t lsb = input_buffer[in_idx++];
                            uint8_t msb = input_buffer[in_idx++];
                            if (format == 0) {
                                blue = (lsb & 0x1F) << 3;
                                green = ((msb & 0x03) << 6) | ((lsb & 0xE0) >> 2);
                                red = (msb & 0x7C) << 1;
                            } else {
                                blue = (lsb & 0x1F) << 3;
                                green = ((msb & 0x07) << 5) | ((lsb & 0xE0) >> 3);
                                red = (msb & 0xF8);
                            }
                            grey = uint8_t((red + green + blue) / 3);
                        } break;
                        case 1:
                        case 2:
                        case 4:
                        case 8: {
                            if (in_bits == 0) {
                                in_byte = input_buffer[in_idx++];
                                in_bits = 8;
                            }
                            uint16_t pn = (in_byte >> bitshift) & bitmask;
                            grey = grey_palette_buffer[pn];
                            in_byte <<= depth;
                            in_bits -= depth;
                        } break;
                        default:
                            valid = false;
                            break;
                    }

                    if (!valid) break;

                    const uint8_t level = grey >> 4; // 0-15
                    output_row_gray_buffer[out_idx++] = level * 17; // expand to 0-255
                }

                if (!valid) break;

                uint16_t yrow = y + (flip ? h - row - 1 : row);
                display.writeNative(output_row_gray_buffer, nullptr, x, yrow, w, 1, false, false, false);

                if ((row % 200) == 0) {
                    LOGD("EINK", "Row %u/%u", (unsigned)row, (unsigned)h);
                }
            }
            LOG_DURATION("EINK", "Rows", rows_start);

            if (valid) {
                const unsigned long refresh_start = millis();
                display.refresh(false);
                LOG_DURATION("EINK", "Refresh", refresh_start);
            }
        }
    }

    LOG_DURATION("EINK", "BMP", start_ms);
    return valid;
}

bool it8951_renderer_init() {
    if (g_display_ready) return true;
    display.init(115200);
    g_display_ready = true;
    LOGI("EINK", "Init OK");
    return true;
}

bool it8951_render_bmp_from_sd(const char *path) {
    if (!path) return false;
    if (!g_display_ready && !it8951_renderer_init()) return false;
    const unsigned long start_ms = millis();
    File file = SD.open(path, FILE_READ);
    if (!file) {
        LOGE("EINK", "BMP open failed");
        return false;
    }

    const bool ok = draw_bmp_16gray(file, 0, 0);
    file.close();
    LOG_DURATION("EINK", "RenderTotal", start_ms);
    return ok;
}

void it8951_renderer_hibernate() {
    if (!g_display_ready) return;
    display.hibernate();
}
