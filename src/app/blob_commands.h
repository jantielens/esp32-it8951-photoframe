#pragma once

#include <Arduino.h>
#include <SPI.h>

#include "config_manager.h"
#include "sd_photo_picker.h"

struct BlobCommandActions {
    bool reboot_now = false;
    bool enter_config_portal_now = false;

    bool override_sleep_seconds = false;
    uint32_t sleep_seconds = 0;

    // When true, command processing should stop for this wake (remaining commands retry next wake).
    bool stop_processing_now = false;

    // When true, caller should skip rendering and go to sleep quickly.
    bool skip_render_and_sleep = false;

    // When true, caller should try to pull more than one blob this wake.
    bool request_resync_from_cloud = false;
};

// Process queued blob commands under the 'commands/' prefix.
//
// Behavior:
// - Lists commands lexicographically (sortable ID in blob name), runs sequentially.
// - Deletes command blob on success.
// - Keeps command blob on failure for retry next wake.
//
// Returns true if it executed at least one command (success or failure).
bool blob_commands_process(DeviceConfig &config, SPIClass &spi, const SdCardPins &pins, uint32_t frequency_hz, BlobCommandActions &out_actions);
