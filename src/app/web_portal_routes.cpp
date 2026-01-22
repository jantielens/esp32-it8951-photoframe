#include "web_portal_routes.h"
#include "web_portal_auth.h"
#include "web_portal_cors.h"
#include "web_portal_config.h"
#include "web_portal_device_api.h"
#include "web_portal_display.h"
#include "web_portal_sd_images.h"
#include "web_portal_render_control_api.h"
#include "web_portal_firmware.h"
#include "web_portal_ota.h"
#include "web_portal_pages.h"

#include "board_config.h"

void web_portal_register_routes(AsyncWebServer* server) {
    auto handleCorsPreflight = [](AsyncWebServerRequest *request) {
        web_portal_send_cors_preflight(request);
    };

    auto registerOptions = [server, handleCorsPreflight](const char* path) {
        server->on(path, HTTP_OPTIONS, handleCorsPreflight);
    };

    // Page routes
    server->on("/", HTTP_GET, handleRoot);
    server->on("/home.html", HTTP_GET, handleHome);
    server->on("/network.html", HTTP_GET, handleNetwork);
    server->on("/firmware.html", HTTP_GET, handleFirmware);

    // Asset routes
    server->on("/portal.css", HTTP_GET, handleCSS);
    server->on("/portal.js", HTTP_GET, handleJS);

    // API endpoints
    // NOTE: Keep more specific routes registered before more general/prefix routes.
    // Some AsyncWebServer matchers can behave like prefix matches depending on configuration.
    registerOptions("/api/mode");
    server->on("/api/mode", HTTP_GET, handleGetMode);

    registerOptions("/api/config");
    server->on("/api/config", HTTP_GET, handleGetConfig);

    server->on(
        "/api/config",
        HTTP_POST,
        [](AsyncWebServerRequest *request) {
            if (!portal_auth_gate(request)) return;
        },
        NULL,
        handlePostConfig
    );

    server->on("/api/config", HTTP_DELETE, handleDeleteConfig);

    registerOptions("/api/info");
    server->on("/api/info", HTTP_GET, handleGetVersion);
    #if HEALTH_HISTORY_ENABLED
    server->on("/api/health/history", HTTP_GET, handleGetHealthHistory);
    #endif
    #if HEALTH_HISTORY_ENABLED
    registerOptions("/api/health/history");
    #endif
    registerOptions("/api/health");
    server->on("/api/health", HTTP_GET, handleGetHealth);

    registerOptions("/api/reboot");
    server->on("/api/reboot", HTTP_POST, handleReboot);

    // Render pause/resume control (for long-running SD operations)
    registerOptions("/api/render/status");
    server->on("/api/render/status", HTTP_GET, handleGetRenderStatus);
    registerOptions("/api/render/pause");
    server->on("/api/render/pause", HTTP_POST, handlePostRenderPause);
    registerOptions("/api/render/resume");
    server->on("/api/render/resume", HTTP_POST, handlePostRenderResume);

    // GitHub Pages-based firmware updates (URL-driven)
    registerOptions("/api/firmware/update/status");
    server->on("/api/firmware/update/status", HTTP_GET, handleGetFirmwareUpdateStatus);
    server->on(
        "/api/firmware/update",
        HTTP_POST,
        [](AsyncWebServerRequest *request) {
            if (!portal_auth_gate(request)) return;
        },
        NULL,
        handlePostFirmwareUpdate
    );
    registerOptions("/api/firmware/update");

#if HAS_BACKLIGHT
    // Display API endpoints
    server->on(
        "/api/display/brightness",
        HTTP_PUT,
        [](AsyncWebServerRequest *request) {
            if (!portal_auth_gate(request)) return;
        },
        NULL,
        handleSetDisplayBrightness
    );
    registerOptions("/api/display/brightness");

    // Runtime-only screen switch
    server->on(
        "/api/display/screen",
        HTTP_PUT,
        [](AsyncWebServerRequest *request) {
            if (!portal_auth_gate(request)) return;
        },
        NULL,
        handleSetDisplayScreen
    );
    registerOptions("/api/display/screen");
#endif

    // OTA upload endpoint
    server->on(
        "/api/update",
        HTTP_POST,
        [](AsyncWebServerRequest *request) {
            if (!portal_auth_gate(request)) return;
        },
        handleOTAUpload
    );
    registerOptions("/api/update");

    // SD image management
    // IMPORTANT: register /api/sd/images/display before /api/sd/images to avoid
    // prefix-matching routing that can misroute display requests to the list handler.
    registerOptions("/api/sd/images/display");
    server->on(
        "/api/sd/images/display",
        HTTP_ANY,
        [](AsyncWebServerRequest *request) {
            if (!portal_auth_gate(request)) return;
            if (request->method() == HTTP_GET || request->method() == HTTP_POST) {
                handleDisplaySdImage(request);
                return;
            }
            request->send(405, "application/json", "{\"success\":false,\"message\":\"Method not allowed\"}");
        }
    );

    registerOptions("/api/sd/images");
    server->on("/api/sd/images", HTTP_GET, handleGetSdImages);
    server->on(
        "/api/sd/images",
        HTTP_POST,
        [](AsyncWebServerRequest *request) {
            if (!portal_auth_gate(request)) return;
        },
        handleUploadSdImage
    );
    server->on("/api/sd/images", HTTP_DELETE, handleDeleteSdImage);

    registerOptions("/api/sd/jobs");
    server->on("/api/sd/jobs", HTTP_GET, handleGetSdJobStatus);

}
