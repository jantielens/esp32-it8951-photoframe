#pragma once

#include <Arduino.h>

bool it8951_renderer_init();
bool it8951_render_bmp_from_sd(const char *path);
void it8951_renderer_hibernate();
