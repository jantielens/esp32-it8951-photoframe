#pragma once

#include "config_manager.h"
#include "sd_photo_picker.h"

void render_scheduler_init(const DeviceConfig &config, uint32_t refresh_interval_ms, uint32_t retry_interval_ms);
void render_scheduler_request_refresh();
void render_scheduler_tick();

typedef bool (*RenderPreEnqueueHook)(void *context);
void render_scheduler_set_pre_enqueue_hook(RenderPreEnqueueHook hook, void *context);

bool render_scheduler_render_once(
    const DeviceConfig &config,
    SPIClass &spi,
    const SdCardPins &pins,
    uint32_t frequency_hz
);
