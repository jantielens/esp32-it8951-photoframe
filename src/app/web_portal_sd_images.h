#pragma once

#include <ESPAsyncWebServer.h>

// SD image management API.
void handleGetSdImages(AsyncWebServerRequest *request);
void handleDeleteSdImage(AsyncWebServerRequest *request);
void handleDisplaySdImage(AsyncWebServerRequest *request);
void handleUploadSdImage(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final);

// SD access guard + queued display handling (call from main loop).
bool sd_images_try_lock(const char *reason);
void sd_images_unlock();
bool sd_images_is_busy();
bool sd_images_process_pending_display();
