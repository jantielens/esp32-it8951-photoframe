#ifndef WEB_PORTAL_DISPLAY_H
#define WEB_PORTAL_DISPLAY_H

#include "board_config.h"

#include <ESPAsyncWebServer.h>

#if HAS_DISPLAY && HAS_BACKLIGHT

void handleSetDisplayBrightness(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total);
void handleSetDisplayScreen(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total);

#endif // HAS_DISPLAY && HAS_BACKLIGHT

#endif // WEB_PORTAL_DISPLAY_H
