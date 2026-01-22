#include "web_portal_display.h"

#if HAS_BACKLIGHT

#include "web_portal_auth.h"
#include "web_portal_state.h"

#include "log_manager.h"
#include "board_config.h"

#include "display_manager.h"

#include <ArduinoJson.h>

void handleSetDisplayBrightness(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    if (!portal_auth_gate(request)) return;

    // Only handle the complete request (index == 0 && index + len == total)
    if (index != 0 || index + len != total) {
        return;
    }

    StaticJsonDocument<128> doc;
    DeserializationError error = deserializeJson(doc, data, len);

    if (error) {
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
        return;
    }

    if (!doc.containsKey("brightness")) {
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Missing brightness\"}");
        return;
    }

    int brightness = doc["brightness"];

    if (brightness < 0) brightness = 0;
    if (brightness > 100) brightness = 100;

    LOGI("API", "PUT /api/display/brightness: %d%%", brightness);

    // Update the in-RAM target brightness (does not persist to NVS).
    // This keeps the screen saver target consistent with what the user sees.
    DeviceConfig *config = web_portal_get_current_config();
    if (config) {
        config->backlight_brightness = brightness;
    }

    display_manager_set_backlight_brightness(brightness);

    char response[64];
    snprintf(response, sizeof(response), "{\"success\":true,\"brightness\":%d}", brightness);
    request->send(200, "application/json", response);
}


void handleSetDisplayScreen(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    if (!portal_auth_gate(request)) return;

    // Only handle the complete request (index == 0 && index + len == total)
    if (index != 0 || index + len != total) {
        return;
    }

    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, data, len);

    if (error) {
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
        return;
    }

    if (!doc.containsKey("screen")) {
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Missing screen ID\"}");
        return;
    }

    const char *screen_id = doc["screen"];
    if (!screen_id || strlen(screen_id) == 0) {
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid screen ID\"}");
        return;
    }

    LOGI("API", "PUT /api/display/screen: %s", screen_id);

    bool success = false;
    display_manager_show_screen(screen_id, &success);

    if (success) {
        char response[96];
        snprintf(response, sizeof(response), "{\"success\":true,\"screen\":\"%s\"}", screen_id);
        request->send(200, "application/json", response);
    } else {
        request->send(404, "application/json", "{\"success\":false,\"message\":\"Screen not found\"}");
    }
}

#endif // HAS_BACKLIGHT
