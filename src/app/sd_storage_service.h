#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <vector>

#include "sd_photo_picker.h"

enum class SdJobType : uint8_t {
    List = 0,
    Delete = 1,
    Upload = 2,
    Display = 3,
    RenderNext = 4,
    SyncFromAzure = 5,
};

enum class SdJobState : uint8_t {
    Queued = 0,
    Running = 1,
    Done = 2,
    Error = 3,
};

struct SdJobInfo {
    uint32_t id = 0;
    SdJobType type = SdJobType::List;
    SdJobState state = SdJobState::Queued;
    bool success = false;
    size_t bytes = 0;
    uint32_t created_ms = 0;
    uint32_t updated_ms = 0;
    char message[96] = {0};
};

bool sd_storage_configure(SPIClass &spi, const SdCardPins &pins, uint32_t frequency_hz);
bool sd_storage_ensure_ready();

uint32_t sd_storage_enqueue_list();
uint32_t sd_storage_enqueue_delete(const char *name);
uint32_t sd_storage_enqueue_upload(const char *name, uint8_t *buffer, size_t size);
uint32_t sd_storage_enqueue_display(const char *name);
uint32_t sd_storage_enqueue_render_next(
    SdImageSelectMode mode,
    uint32_t last_index,
    const char *last_name
);

// Re-sync SD contents from Azure Blob Storage. Intended for manual recovery.
// Downloads blobs from all/temporary and all/permanent, excluding queued items
// and expired temporaries when time is valid, then writes them to SD.
uint32_t sd_storage_enqueue_sync_from_azure(const char *container_sas_url);

bool sd_storage_get_job(uint32_t id, SdJobInfo *out);
bool sd_storage_get_job_names(uint32_t id, std::vector<String> &out_names);

void sd_storage_purge_jobs();
