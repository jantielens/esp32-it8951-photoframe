#pragma once

#include <Arduino.h>

bool it8951_renderer_init();
bool it8951_renderer_is_busy();
bool it8951_render_bmp_from_sd(const char *path);
bool it8951_convert_bmp_to_raw_g4(const char *bmp_path, const char *raw_path, const char *g4_path);
bool it8951_render_raw8(const char *raw_path);
bool it8951_render_g4(const char *g4_path);
bool it8951_render_g4_buffer(const uint8_t* g4, uint16_t w, uint16_t h);
bool it8951_render_g4_buffer_ex(const uint8_t* g4, uint16_t w, uint16_t h, bool full_refresh);
bool it8951_render_g4_buffer_region(const uint8_t* g4, uint16_t panel_w, uint16_t panel_h,
									uint16_t x, uint16_t y, uint16_t w, uint16_t h);
bool it8951_render_g4_region(const uint8_t* g4_region, uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool full_refresh);
bool it8951_render_full_white();
void it8951_renderer_hibernate();
