#pragma once

#include <ESPAsyncWebServer.h>

// Archive-backed preview proxy (thumb-only for now).
void handleGetArchivePreview(AsyncWebServerRequest *request);
