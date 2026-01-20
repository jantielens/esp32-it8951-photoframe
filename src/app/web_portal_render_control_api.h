#ifndef WEB_PORTAL_RENDER_CONTROL_API_H
#define WEB_PORTAL_RENDER_CONTROL_API_H

#include <ESPAsyncWebServer.h>

void handlePostRenderPause(AsyncWebServerRequest *request);
void handlePostRenderResume(AsyncWebServerRequest *request);
void handleGetRenderStatus(AsyncWebServerRequest *request);

#endif // WEB_PORTAL_RENDER_CONTROL_API_H
