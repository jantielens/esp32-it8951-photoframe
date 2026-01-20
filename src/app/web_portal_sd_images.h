#pragma once

#include <ESPAsyncWebServer.h>

// SD image management API (async job-based).
void handleGetSdImages(AsyncWebServerRequest *request);
void handleDeleteSdImage(AsyncWebServerRequest *request);
void handleDisplaySdImage(AsyncWebServerRequest *request);
void handleUploadSdImage(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final);
void handleGetSdJobStatus(AsyncWebServerRequest *request);
