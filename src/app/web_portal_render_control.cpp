#include "web_portal_render_control.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static portMUX_TYPE g_render_mux = portMUX_INITIALIZER_UNLOCKED;
static bool g_render_paused = false;

void web_portal_render_set_paused(bool paused) {
    portENTER_CRITICAL(&g_render_mux);
    g_render_paused = paused;
    portEXIT_CRITICAL(&g_render_mux);
}

bool web_portal_render_is_paused() {
    portENTER_CRITICAL(&g_render_mux);
    const bool paused = g_render_paused;
    portEXIT_CRITICAL(&g_render_mux);
    return paused;
}
