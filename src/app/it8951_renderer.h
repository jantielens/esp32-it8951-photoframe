#pragma once

#include <Arduino.h>

bool it8951_renderer_init();
bool it8951_render_bmp_from_sd(const char *path);
bool it8951_convert_bmp_to_raw_g4(const char *bmp_path, const char *raw_path, const char *g4_path);
bool it8951_render_raw8(const char *raw_path);
bool it8951_render_g4(const char *g4_path);
void it8951_renderer_hibernate();
