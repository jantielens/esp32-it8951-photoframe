#include "web_portal_render_control_api.h"

#include "web_portal_auth.h"
#include "web_portal_json.h"
#include "web_portal_render_control.h"

void handlePostRenderPause(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;
    web_portal_render_set_paused(true);
    request->send(200, "application/json", "{\"success\":true,\"paused\":true}");
}

void handlePostRenderResume(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;
    web_portal_render_set_paused(false);
    request->send(200, "application/json", "{\"success\":true,\"paused\":false}");
}

void handleGetRenderStatus(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;
    const bool paused = web_portal_render_is_paused();
    const char *payload = paused
        ? "{\"success\":true,\"paused\":true}"
        : "{\"success\":true,\"paused\":false}";
    request->send(200, "application/json", payload);
}
