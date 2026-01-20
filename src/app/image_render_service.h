#pragma once

#include <Arduino.h>
#include "sd_photo_picker.h"

// Central image render pipeline: priority override + sequential/random selection.
// Returns true if an image was rendered successfully.
bool image_render_service_render_next(SdImageSelectMode mode, uint32_t last_index, const char *last_name);
