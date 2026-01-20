#include "it8951_renderer.h"

#include "board_config.h"
#include "display_manager.h"
#include "log_manager.h"

#include <SD.h>
#include <GxEPD2.h>
#include <it8951/GxEPD2_it78_1872x1404.h>
#include <esp_heap_caps.h>

// ---------------------------------------------------------------------------
// Display wiring (from board overrides)
// ---------------------------------------------------------------------------
GxEPD2_it78_1872x1404 display(IT8951_CS_PIN, IT8951_DC_PIN, IT8951_RST_PIN, IT8951_BUSY_PIN);

static bool g_display_ready = false;
static volatile bool g_render_busy = false;
static portMUX_TYPE g_render_mux = portMUX_INITIALIZER_UNLOCKED;

static const uint16_t kInputBufferPixels = 1872;
static const uint16_t kMaxRowWidth = 1872;
static const uint16_t kMaxPalettePixels = 256;
static const uint16_t kChunkRows = 16; // number of rows per writeNative
static const size_t kInputBufferBytes = 3 * kInputBufferPixels;

static uint8_t *input_buffer = nullptr;
static uint8_t *output_rows_gray_buffer = nullptr;
static uint8_t *grey_palette_buffer = nullptr;
static uint8_t *raw_row_buffer = nullptr;
static uint8_t *g4_row_buffer = nullptr;
static uint8_t *g4_chunk_buffer = nullptr;

static bool buffers_ready = false;
static bool buffers_logged = false;

static void *alloc_buffer(size_t bytes, const char *label) {
    void *ptr = nullptr;
    if (psramFound()) {
        ptr = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!ptr) {
        ptr = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!ptr) {
        LOGE("EINK", "Buffer alloc failed: %s (%u bytes)", label, (unsigned)bytes);
    }
    return ptr;
}

static bool ensure_buffers() {
    if (buffers_ready) return true;

    input_buffer = static_cast<uint8_t*>(alloc_buffer(kInputBufferBytes, "input"));
    output_rows_gray_buffer = static_cast<uint8_t*>(alloc_buffer(kMaxRowWidth * kChunkRows, "rows"));
    grey_palette_buffer = static_cast<uint8_t*>(alloc_buffer(kMaxPalettePixels, "palette"));
    raw_row_buffer = static_cast<uint8_t*>(alloc_buffer(kMaxRowWidth, "raw"));
    g4_row_buffer = static_cast<uint8_t*>(alloc_buffer(kMaxRowWidth / 2, "g4_row"));
    g4_chunk_buffer = static_cast<uint8_t*>(alloc_buffer((kMaxRowWidth / 2) * kChunkRows, "g4_chunk"));

    buffers_ready = input_buffer && output_rows_gray_buffer && grey_palette_buffer && raw_row_buffer && g4_row_buffer && g4_chunk_buffer;
    if (buffers_ready && !buffers_logged) {
        buffers_logged = true;
        auto log_buf = [](const char *label, const void *ptr) {
            const bool is_psram = ptr && esp_ptr_external_ram(ptr);
            LOGI("EINK", "%s buffer: %s", label, is_psram ? "PSRAM" : "internal");
        };
        log_buf("input", input_buffer);
        log_buf("rows", output_rows_gray_buffer);
        log_buf("palette", grey_palette_buffer);
        log_buf("raw", raw_row_buffer);
        log_buf("g4_row", g4_row_buffer);
        log_buf("g4_chunk", g4_chunk_buffer);
    }
    return buffers_ready;
}

static void set_render_busy(bool busy) {
    portENTER_CRITICAL(&g_render_mux);
    g_render_busy = busy;
    portEXIT_CRITICAL(&g_render_mux);
}

static bool is_ui_active() {
#if HAS_DISPLAY
    return display_manager_ui_is_active();
#else
    return false;
#endif
}

bool it8951_renderer_is_busy() {
    bool busy = false;
    portENTER_CRITICAL(&g_render_mux);
    busy = g_render_busy;
    portEXIT_CRITICAL(&g_render_mux);
    return busy;
}

// IT8951 I80 command constants (mirroring GxEPD2 driver)
static const uint16_t IT8951_TCON_SYS_RUN = 0x0001;
static const uint16_t IT8951_TCON_LD_IMG_AREA = 0x0021;
static const uint16_t IT8951_TCON_LD_IMG_END = 0x0022;

static const uint16_t IT8951_ROTATE_0 = 0;
static const uint16_t IT8951_4BPP = 2;
static const uint16_t IT8951_LDIMG_B_ENDIAN = 1;

static const uint32_t kBusyTimeoutUs = 10000000;

static SPISettings it8951_spi_settings(24000000, MSBFIRST, SPI_MODE0);

static void it8951_wait_ready(uint16_t busy_time_ms = 1) {
    if (IT8951_BUSY_PIN >= 0) {
        const unsigned long start = micros();
        while (digitalRead(IT8951_BUSY_PIN) == LOW) {
            delay(1);
            if (micros() - start > kBusyTimeoutUs) {
                LOGW("EINK", "IT8951 busy timeout");
                break;
            }
        }
    } else {
        delay(busy_time_ms);
    }
}

static uint16_t it8951_transfer16(uint16_t value) {
    uint16_t rv = SPI.transfer(value >> 8) << 8;
    return (rv | SPI.transfer(value));
}

static void it8951_write_command16(uint16_t cmd) {
    it8951_wait_ready();
    SPI.beginTransaction(it8951_spi_settings);
    digitalWrite(IT8951_CS_PIN, LOW);
    it8951_transfer16(0x6000);
    it8951_wait_ready();
    it8951_transfer16(cmd);
    digitalWrite(IT8951_CS_PIN, HIGH);
    SPI.endTransaction();
}

static void it8951_write_data16(uint16_t data) {
    it8951_wait_ready();
    SPI.beginTransaction(it8951_spi_settings);
    digitalWrite(IT8951_CS_PIN, LOW);
    it8951_transfer16(0x0000);
    it8951_wait_ready();
    it8951_transfer16(data);
    digitalWrite(IT8951_CS_PIN, HIGH);
    SPI.endTransaction();
}

static void it8951_write_command_data16(uint16_t cmd, const uint16_t* data, uint16_t count) {
    it8951_write_command16(cmd);
    for (uint16_t i = 0; i < count; i++) {
        it8951_write_data16(data[i]);
    }
}

static void it8951_set_partial_area_4bpp(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    uint16_t args[5];
    args[0] = (IT8951_LDIMG_B_ENDIAN << 8) | (IT8951_4BPP << 4) | (IT8951_ROTATE_0);
    args[1] = x;
    args[2] = y;
    args[3] = w;
    args[4] = h;
    it8951_write_command_data16(IT8951_TCON_LD_IMG_AREA, args, 5);
}

static void it8951_write_data_bytes(const uint8_t* data, size_t length) {
    it8951_wait_ready();
    SPI.beginTransaction(it8951_spi_settings);
    digitalWrite(IT8951_CS_PIN, LOW);
    it8951_transfer16(0x0000);
    it8951_wait_ready();
#if defined(ARDUINO_ARCH_ESP32)
    SPI.writeBytes(data, length);
#else
    for (size_t i = 0; i < length; i++) {
        SPI.transfer(data[i]);
    }
#endif
    digitalWrite(IT8951_CS_PIN, HIGH);
    SPI.endTransaction();
}

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
                        in_bytes = file.read(input_buffer, in_remain > kInputBufferBytes ? kInputBufferBytes : in_remain);
                        in_remain -= in_bytes;
                        in_idx = 0;
                    }

                    switch (depth) {
                        case 32:
                            blue = input_buffer[in_idx++];
                            green = input_buffer[in_idx++];
                            red = input_buffer[in_idx++];
                            in_idx++;
                            grey = uint8_t((red * 77 + green * 150 + blue * 29) >> 8);
                            break;
                        case 24:
                            blue = input_buffer[in_idx++];
                            green = input_buffer[in_idx++];
                            red = input_buffer[in_idx++];
                            grey = uint8_t((red * 77 + green * 150 + blue * 29) >> 8);
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
                            grey = uint8_t((red * 77 + green * 150 + blue * 29) >> 8);
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
                    output_rows_gray_buffer[(row % kChunkRows) * kMaxRowWidth + out_idx++] = level * 17; // expand to 0-255
                }

                if (!valid) break;

                const bool chunk_ready = ((row % kChunkRows) == (kChunkRows - 1)) || (row == (h - 1));
                if (chunk_ready) {
                    const uint16_t chunk_rows = (row % kChunkRows) + 1;
                    const uint16_t yrow = y + (flip ? (h - row - chunk_rows) : (row - chunk_rows + 1));
                    display.writeNative(output_rows_gray_buffer, nullptr, x, yrow, w, chunk_rows, false, false, false);
                }

                if ((row % 200) == 0) {
                    LOGD("EINK", "Row %u/%u", (unsigned)row, (unsigned)h);
                }

                if ((row % 32) == 0) {
                    yield();
                }
            }
            LOG_DURATION("EINK", "Rows", rows_start);

            if (valid) {
                const unsigned long refresh_start = millis();
                display.refresh(true);
                LOG_DURATION("EINK", "Refresh", refresh_start);
            }
        }
    }

    LOG_DURATION("EINK", "BMP", start_ms);
    return valid;
}

static bool render_raw_rows(File &file, uint16_t w, uint16_t h) {
    const unsigned long rows_start = millis();
    for (uint16_t row = 0; row < h; row++) {
        const int read_bytes = file.read(raw_row_buffer, w);
        if (read_bytes != (int)w) {
            LOGE("EINK", "RAW short read row=%u bytes=%d", (unsigned)row, read_bytes);
            return false;
        }

        memcpy(&output_rows_gray_buffer[(row % kChunkRows) * kMaxRowWidth], raw_row_buffer, w);

        const bool chunk_ready = ((row % kChunkRows) == (kChunkRows - 1)) || (row == (h - 1));
        if (chunk_ready) {
            const uint16_t chunk_rows = (row % kChunkRows) + 1;
            const uint16_t yrow = row - chunk_rows + 1;
            display.writeNative(output_rows_gray_buffer, nullptr, 0, yrow, w, chunk_rows, false, false, false);
        }

        if ((row % 200) == 0) {
            LOGD("EINK", "RAW Row %u/%u", (unsigned)row, (unsigned)h);
        }

        if ((row % 32) == 0) {
            yield();
        }
    }
    LOG_DURATION("EINK", "Rows", rows_start);
    return true;
}

static bool render_g4_rows(File &file, uint16_t w, uint16_t h) {
    const unsigned long rows_start = millis();
    const uint16_t packed_width = w / 2;
    it8951_write_command16(IT8951_TCON_SYS_RUN);
    for (uint16_t row = 0; row < h; row++) {
        const uint16_t chunk_offset = (row % kChunkRows) * packed_width;
        const int read_bytes = file.read(&g4_chunk_buffer[chunk_offset], packed_width);
        if (read_bytes != (int)packed_width) {
            LOGE("EINK", "G4 short read row=%u bytes=%d", (unsigned)row, read_bytes);
            return false;
        }

        const bool chunk_ready = ((row % kChunkRows) == (kChunkRows - 1)) || (row == (h - 1));
        if (chunk_ready) {
            const uint16_t chunk_rows = (row % kChunkRows) + 1;
            const uint16_t yrow = row - chunk_rows + 1;
            const size_t chunk_bytes = (size_t)chunk_rows * packed_width;
            it8951_set_partial_area_4bpp(0, yrow, w, chunk_rows);
            it8951_write_data_bytes(g4_chunk_buffer, chunk_bytes);
            it8951_write_command16(IT8951_TCON_LD_IMG_END);
        }

        if ((row % 200) == 0) {
            LOGD("EINK", "G4 Row %u/%u", (unsigned)row, (unsigned)h);
        }

        if ((row % 32) == 0) {
            yield();
        }
    }
    LOG_DURATION("EINK", "Rows", rows_start);
    return true;
}

bool it8951_renderer_init() {
    if (g_display_ready) return true;
    if (!ensure_buffers()) {
        LOGE("EINK", "Init failed (buffers)");
        return false;
    }
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

bool it8951_convert_bmp_to_raw_g4(const char *bmp_path, const char *raw_path, const char *g4_path) {
    if (!bmp_path || !raw_path || !g4_path) return false;
    if (!ensure_buffers()) return false;
    File bmp = SD.open(bmp_path, FILE_READ);
    if (!bmp) {
        LOGE("EINK", "BMP open failed path=%s", bmp_path);
        return false;
    }

    const unsigned long start_ms = millis();

    uint16_t signature = read16(bmp);
    if (signature != 0x4D42) {
        LOGE("EINK", "BMP signature mismatch");
        bmp.close();
        return false;
    }

    (void)read32(bmp); // file size
    (void)read32(bmp); // creator bytes
    uint32_t image_offset = read32(bmp);
    (void)read32(bmp); // header size
    uint32_t width = read32(bmp);
    int32_t height = (int32_t)read32(bmp);
    uint16_t planes = read16(bmp);
    uint16_t depth = read16(bmp);
    uint32_t format = read32(bmp);

    if ((planes != 1) || !((format == 0) || (format == 3))) {
        LOGE("EINK", "BMP format unsupported");
        bmp.close();
        return false;
    }

    if (height < 0) {
        height = -height;
    }

    if (width != display.WIDTH || height != display.HEIGHT) {
        LOGE("EINK", "BMP size mismatch %lux%ld", (unsigned long)width, (long)height);
        bmp.close();
        return false;
    }

    uint32_t row_size = (width * depth / 8 + 3) & ~3;
    if (depth < 8) {
        row_size = ((width * depth + 8 - depth) / 8 + 3) & ~3;
    }

    uint8_t bitmask = 0xFF;
    uint8_t bitshift = 8 - depth;
    uint16_t red, green, blue;
    uint8_t grey;

    if (depth <= 8) {
        if (depth < 8) bitmask >>= depth;
        bmp.seek(image_offset - (4 << depth));
        for (uint16_t pn = 0; pn < (1 << depth); pn++) {
            blue = read8(bmp);
            green = read8(bmp);
            red = read8(bmp);
            read8(bmp);
            grey = uint8_t((red * 77 + green * 150 + blue * 29) >> 8);
            grey_palette_buffer[pn] = grey;
        }
    }

    File raw = SD.open(raw_path, FILE_WRITE);
    File g4 = SD.open(g4_path, FILE_WRITE);
    if (!raw || !g4) {
        LOGE("EINK", "RAW/G4 open failed");
        bmp.close();
        if (raw) raw.close();
        if (g4) g4.close();
        return false;
    }

    bmp.seek(image_offset);

    for (uint16_t row = 0; row < height; row++) {
        uint32_t in_remain = row_size;
        uint32_t in_idx = 0;
        uint32_t in_bytes = 0;
        uint8_t in_byte = 0;
        uint8_t in_bits = 0;

        uint32_t raw_idx = 0;
        uint32_t g4_idx = 0;
        uint8_t g4_pack = 0;

        for (uint16_t col = 0; col < width; col++) {
            if (in_idx >= in_bytes) {
                in_bytes = bmp.read(input_buffer, in_remain > kInputBufferBytes ? kInputBufferBytes : in_remain);
                in_remain -= in_bytes;
                in_idx = 0;
            }

            switch (depth) {
                case 32:
                    blue = input_buffer[in_idx++];
                    green = input_buffer[in_idx++];
                    red = input_buffer[in_idx++];
                    in_idx++;
                    grey = uint8_t((red * 77 + green * 150 + blue * 29) >> 8);
                    break;
                case 24:
                    blue = input_buffer[in_idx++];
                    green = input_buffer[in_idx++];
                    red = input_buffer[in_idx++];
                    grey = uint8_t((red * 77 + green * 150 + blue * 29) >> 8);
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
                    grey = uint8_t((red * 77 + green * 150 + blue * 29) >> 8);
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
                    bmp.close();
                    raw.close();
                    g4.close();
                    LOGE("EINK", "BMP depth unsupported");
                    return false;
            }

            const uint8_t level = grey >> 4;
            raw_row_buffer[raw_idx++] = level * 17;

            if ((col & 1) == 0) {
                g4_pack = (level << 4);
            } else {
                g4_pack |= level;
                g4_row_buffer[g4_idx++] = g4_pack;
            }
        }

        raw.write(raw_row_buffer, width);
        g4.write(g4_row_buffer, width / 2);

        if ((row % 200) == 0) {
            LOGD("EINK", "CONV Row %u/%lu", (unsigned)row, (unsigned long)height);
        }
    }

    bmp.close();
    raw.close();
    g4.close();
    LOG_DURATION("EINK", "Convert", start_ms);
    return true;
}

bool it8951_render_raw8(const char *raw_path) {
    if (!raw_path) return false;
    if (!g_display_ready && !it8951_renderer_init()) return false;
    if (it8951_renderer_is_busy()) return false;
    set_render_busy(true);
    File raw = SD.open(raw_path, FILE_READ);
    if (!raw) {
        LOGE("EINK", "RAW open failed");
        set_render_busy(false);
        return false;
    }

    const unsigned long start_ms = millis();
    const uint16_t w = display.WIDTH;
    const uint16_t h = display.HEIGHT;
    const bool ok = render_raw_rows(raw, w, h);
    raw.close();

    if (ok) {
        const unsigned long refresh_start = millis();
        display.refresh(false);
        LOG_DURATION("EINK", "Refresh", refresh_start);
    }

    LOG_DURATION("EINK", "RenderRaw", start_ms);
    set_render_busy(false);
    return ok;
}

bool it8951_render_g4(const char *g4_path) {
    if (!g4_path) return false;
    if (is_ui_active()) {
        LOGE("EINK", "Render blocked: UI active. Call display_manager_ui_stop() before rendering.");
        return false;
    }
    if (!g_display_ready && !it8951_renderer_init()) return false;
    if (it8951_renderer_is_busy()) return false;
    set_render_busy(true);
    File g4 = SD.open(g4_path, FILE_READ);
    if (!g4) {
        LOGE("EINK", "G4 open failed");
        set_render_busy(false);
        return false;
    }

    const unsigned long start_ms = millis();
    const uint16_t w = display.WIDTH;
    const uint16_t h = display.HEIGHT;
    const bool ok = render_g4_rows(g4, w, h);
    g4.close();

    if (ok) {
        const unsigned long refresh_start = millis();
        display.refresh(false);
        LOG_DURATION("EINK", "Refresh", refresh_start);
    }

    LOG_DURATION("EINK", "RenderG4", start_ms);
    set_render_busy(false);
    return ok;
}

bool it8951_render_g4_buffer(const uint8_t* g4, uint16_t w, uint16_t h) {
    return it8951_render_g4_buffer_ex(g4, w, h, true);
}

bool it8951_render_g4_buffer_ex(const uint8_t* g4, uint16_t w, uint16_t h, bool full_refresh) {
    if (!g4) return false;
    if (is_ui_active()) {
        LOGE("EINK", "Render blocked: UI active. Call display_manager_ui_stop() before rendering.");
        return false;
    }
    if (!g_display_ready && !it8951_renderer_init()) return false;
    if (it8951_renderer_is_busy()) return false;
    set_render_busy(true);

    if (w != display.WIDTH || h != display.HEIGHT) {
        LOGW("EINK", "G4 buffer size mismatch %ux%u (panel %ux%u)",
             (unsigned)w, (unsigned)h, (unsigned)display.WIDTH, (unsigned)display.HEIGHT);
    }

    const unsigned long start_ms = millis();
    const uint16_t packed_width = w / 2;

    it8951_write_command16(IT8951_TCON_SYS_RUN);

    for (uint16_t row = 0; row < h; row += kChunkRows) {
        const uint16_t chunk_rows = (uint16_t)min((uint16_t)kChunkRows, (uint16_t)(h - row));
        const size_t chunk_bytes = (size_t)chunk_rows * packed_width;
        const uint8_t* chunk_ptr = g4 + (size_t)row * packed_width;

        it8951_set_partial_area_4bpp(0, row, w, chunk_rows);
        it8951_write_data_bytes(chunk_ptr, chunk_bytes);
        it8951_write_command16(IT8951_TCON_LD_IMG_END);

        if ((row % 200) == 0) {
            LOGD("EINK", "G4 buf Row %u/%u", (unsigned)row, (unsigned)h);
        }

        if ((row % 32) == 0) {
            yield();
        }
    }

    const unsigned long refresh_start = millis();
    display.refresh(full_refresh);
    LOG_DURATION("EINK", "Refresh", refresh_start);

    LOG_DURATION("EINK", "RenderG4Buf", start_ms);
    set_render_busy(false);
    return true;
}

bool it8951_render_g4_buffer_region(const uint8_t* g4, uint16_t panel_w, uint16_t panel_h,
                                    uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (!g4) return false;
    if (w == 0 || h == 0) return false;
    if (!g_display_ready && !it8951_renderer_init()) return false;
    if (it8951_renderer_is_busy()) return false;

    if (x >= panel_w || y >= panel_h) return false;

    // Clamp region within panel bounds.
    if (x + w > panel_w) w = panel_w - x;
    if (y + h > panel_h) h = panel_h - y;

    // 4bpp packed: align x and width to even pixel boundaries.
    if (x & 1U) {
        x -= 1;
        if (w + 1 <= panel_w) w += 1;
    }
    if (w & 1U) {
        if (x + w + 1 <= panel_w) {
            w += 1;
        } else if (w > 1) {
            w -= 1;
        }
    }

    if (w == 0 || h == 0) return false;

    set_render_busy(true);
    const unsigned long start_ms = millis();
    const uint16_t packed_width = panel_w / 2;
    const uint16_t region_bytes_per_row = w / 2;

    it8951_write_command16(IT8951_TCON_SYS_RUN);

    for (uint16_t row = 0; row < h; row += kChunkRows) {
        const uint16_t chunk_rows = (uint16_t)min((uint16_t)kChunkRows, (uint16_t)(h - row));
        const uint16_t yrow = (uint16_t)(y + row);

        // Pack region rows into contiguous chunk buffer.
        for (uint16_t r = 0; r < chunk_rows; r++) {
            const uint32_t src_row = (uint32_t)(yrow + r);
            const uint32_t src_offset = src_row * packed_width + (x / 2);
            const uint32_t dst_offset = (uint32_t)r * region_bytes_per_row;
            memcpy(&g4_chunk_buffer[dst_offset], &g4[src_offset], region_bytes_per_row);
        }

        const size_t chunk_bytes = (size_t)chunk_rows * region_bytes_per_row;
        it8951_set_partial_area_4bpp(x, yrow, w, chunk_rows);
        it8951_write_data_bytes(g4_chunk_buffer, chunk_bytes);
        it8951_write_command16(IT8951_TCON_LD_IMG_END);

        if ((row % 200) == 0) {
            LOGD("EINK", "G4 buf region Row %u/%u", (unsigned)row, (unsigned)h);
        }

        if ((row % 32) == 0) {
            yield();
        }
    }

    const unsigned long refresh_start = millis();
    display.refresh(true);
    LOG_DURATION("EINK", "Refresh", refresh_start);

    LOG_DURATION("EINK", "RenderG4BufRegion", start_ms);
    set_render_busy(false);
    return true;
}

bool it8951_render_g4_region(const uint8_t* g4_region, uint16_t x, uint16_t y,
                             uint16_t w, uint16_t h, bool full_refresh) {
    if (!g4_region) return false;
    if (w == 0 || h == 0) return false;
    if (x & 1U || w & 1U) {
        LOGW("EINK", "G4 region requires even x/width (x=%u w=%u)", (unsigned)x, (unsigned)w);
        return false;
    }
    if (!g_display_ready && !it8951_renderer_init()) return false;
    if (it8951_renderer_is_busy()) return false;

    set_render_busy(true);
    const unsigned long start_ms = millis();
    const uint16_t packed_width = w / 2;

    it8951_write_command16(IT8951_TCON_SYS_RUN);

    for (uint16_t row = 0; row < h; row += kChunkRows) {
        const uint16_t chunk_rows = (uint16_t)min((uint16_t)kChunkRows, (uint16_t)(h - row));
        const uint16_t yrow = (uint16_t)(y + row);
        const size_t chunk_bytes = (size_t)chunk_rows * packed_width;
        const uint8_t* chunk_ptr = g4_region + (size_t)row * packed_width;

        it8951_set_partial_area_4bpp(x, yrow, w, chunk_rows);
        it8951_write_data_bytes(chunk_ptr, chunk_bytes);
        it8951_write_command16(IT8951_TCON_LD_IMG_END);

        if ((row % 200) == 0) {
            LOGD("EINK", "G4 region Row %u/%u", (unsigned)row, (unsigned)h);
        }

        if ((row % 32) == 0) {
            yield();
        }
    }

    const unsigned long refresh_start = millis();
    if (full_refresh) {
        display.refresh(true);
    } else {
        display.refresh((int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h);
    }
    LOG_DURATION("EINK", "Refresh", refresh_start);

    LOG_DURATION("EINK", "RenderG4Region", start_ms);
    set_render_busy(false);
    return true;
}

void it8951_renderer_hibernate() {
    if (!g_display_ready) return;
    display.hibernate();
}

bool it8951_render_full_white() {
    if (!g_display_ready && !it8951_renderer_init()) return false;
    if (it8951_renderer_is_busy()) return false;

    set_render_busy(true);
    const unsigned long start_ms = millis();

    display.clearScreen();
    display.refresh(false);

    LOG_DURATION("EINK", "FullWhite", start_ms);
    set_render_busy(false);
    return true;
}
