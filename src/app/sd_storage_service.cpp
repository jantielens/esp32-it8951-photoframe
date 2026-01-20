#include "sd_storage_service.h"

#include "log_manager.h"
#include "rtc_state.h"
#include "it8951_renderer.h"
#include "image_render_service.h"

#include <SD.h>
#include <vector>
#include <algorithm>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/portmacro.h>

#include <esp_heap_caps.h>

namespace {
static constexpr size_t kMaxJobs = 16;
static constexpr size_t kMaxNameLen = 63;
static constexpr uint32_t kJobGcMinAgeMs = 60000;
static constexpr uint32_t kWorkerStackSize = 8192;
static constexpr UBaseType_t kWorkerPriority = 2;
static constexpr size_t kJobQueueDepth = 8;
#if defined(portNUM_PROCESSORS) && (portNUM_PROCESSORS > 1)
static constexpr BaseType_t kWorkerCore = 1;
#else
static constexpr BaseType_t kWorkerCore = 0;
#endif

struct SdJob {
    uint32_t id = 0;
    SdJobType type = SdJobType::List;
    SdJobState state = SdJobState::Queued;
    bool success = false;
    size_t bytes = 0;
    uint32_t created_ms = 0;
    uint32_t updated_ms = 0;
    char message[96] = {0};

    char name[kMaxNameLen + 1] = {0};
    uint8_t *buffer = nullptr;
    size_t buffer_size = 0;

    SdImageSelectMode mode = SdImageSelectMode::Random;
    uint32_t last_index = 0;
    char last_name[kMaxNameLen + 1] = {0};

    std::vector<String> names;
};

static SPIClass *g_spi = nullptr;
static SdCardPins g_pins = {0, 0, 0, 0, -1};
static uint32_t g_frequency = 0;
static bool g_sd_ready = false;

static QueueHandle_t g_job_queue = nullptr;
static TaskHandle_t g_worker_task = nullptr;

static portMUX_TYPE g_jobs_mux = portMUX_INITIALIZER_UNLOCKED;
static SdJob *g_jobs[kMaxJobs] = {nullptr};
static uint32_t g_next_job_id = 1;

static void job_set_message(SdJob *job, const char *msg) {
    if (!job) return;
    if (msg) {
        strlcpy(job->message, msg, sizeof(job->message));
    } else {
        job->message[0] = '\0';
    }
}

static SdJob *alloc_job() {
    SdJob *job = new SdJob();
    if (!job) return nullptr;
    job->id = g_next_job_id++;
    job->created_ms = millis();
    job->updated_ms = job->created_ms;
    return job;
}

static void free_job(SdJob *job) {
    if (!job) return;
    if (job->buffer) {
        heap_caps_free(job->buffer);
        job->buffer = nullptr;
    }
    delete job;
}

static void gc_jobs_locked() {
    const uint32_t now = millis();
    for (size_t i = 0; i < kMaxJobs; i++) {
        SdJob *job = g_jobs[i];
        if (!job) continue;
        if (job->state == SdJobState::Queued || job->state == SdJobState::Running) continue;
        if (now - job->updated_ms < kJobGcMinAgeMs) continue;
        free_job(job);
        g_jobs[i] = nullptr;
    }
}

static bool store_job(SdJob *job) {
    if (!job) return false;
    portENTER_CRITICAL(&g_jobs_mux);
    gc_jobs_locked();
    for (size_t i = 0; i < kMaxJobs; i++) {
        if (!g_jobs[i]) {
            g_jobs[i] = job;
            portEXIT_CRITICAL(&g_jobs_mux);
            return true;
        }
    }

    // No free slot - evict oldest completed job.
    size_t oldest_idx = kMaxJobs;
    uint32_t oldest_ms = UINT32_MAX;
    for (size_t i = 0; i < kMaxJobs; i++) {
        SdJob *candidate = g_jobs[i];
        if (!candidate) continue;
        if (candidate->state == SdJobState::Queued || candidate->state == SdJobState::Running) continue;
        if (candidate->updated_ms < oldest_ms) {
            oldest_ms = candidate->updated_ms;
            oldest_idx = i;
        }
    }

    if (oldest_idx < kMaxJobs) {
        free_job(g_jobs[oldest_idx]);
        g_jobs[oldest_idx] = job;
        portEXIT_CRITICAL(&g_jobs_mux);
        return true;
    }

    portEXIT_CRITICAL(&g_jobs_mux);
    return false;
}

static SdJob *find_job(uint32_t id) {
    SdJob *job = nullptr;
    portENTER_CRITICAL(&g_jobs_mux);
    for (size_t i = 0; i < kMaxJobs; i++) {
        if (g_jobs[i] && g_jobs[i]->id == id) {
            job = g_jobs[i];
            break;
        }
    }
    portEXIT_CRITICAL(&g_jobs_mux);
    return job;
}

static bool ensure_sd_ready_internal() {
    if (g_sd_ready) return true;
    if (!g_spi) return false;
    if (!sd_photo_picker_init(*g_spi, g_pins, g_frequency)) {
        return false;
    }
    g_sd_ready = true;
    return true;
}

static bool is_valid_g4_name(const char *name) {
    if (!name) return false;
    const size_t len = strlen(name);
    if (len == 0 || len > kMaxNameLen) return false;
    if (strchr(name, '/') || strchr(name, '\\')) return false;
    return (len >= 3 && strcmp(name + (len - 3), ".g4") == 0);
}

static bool collect_g4_names(std::vector<String> &names) {
    File root = SD.open("/");
    if (!root) return false;
    if (!root.isDirectory()) {
        root.close();
        return false;
    }

    File file = root.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            const char *name = file.name();
            if (is_valid_g4_name(name)) {
                names.push_back(String(name));
            }
        }
        file.close();
        file = root.openNextFile();
    }

    root.close();
    return true;
}

static void sort_names(std::vector<String> &names) {
    std::sort(names.begin(), names.end(), [](const String &a, const String &b) {
        return a.compareTo(b) < 0;
    });
}

static bool write_upload_to_sd(SdJob *job) {
    if (!job || !job->buffer || job->buffer_size == 0) return false;
    if (!is_valid_g4_name(job->name)) {
        job_set_message(job, "Invalid filename");
        return false;
    }

    const String target_path = "/" + String(job->name);
    const String temp_path = target_path + ".tmp";

    LOGI("SDJob", "Upload start name=%s bytes=%u", job->name, (unsigned)job->buffer_size);

    if (SD.exists(temp_path)) {
        SD.remove(temp_path);
    }

    File file = SD.open(temp_path, FILE_WRITE);
    if (!file) {
        job_set_message(job, "Open failed");
        LOGE("SDJob", "Upload open failed %s", temp_path.c_str());
        return false;
    }

    const size_t written = file.write(job->buffer, job->buffer_size);
    file.flush();
    file.close();

    job->bytes = written;

    if (written != job->buffer_size) {
        SD.remove(temp_path);
        job_set_message(job, "Write failed");
        LOGE("SDJob", "Upload write failed %s", temp_path.c_str());
        return false;
    }

    if (SD.exists(target_path)) {
        SD.remove(target_path);
    }

    if (!SD.rename(temp_path, target_path)) {
        SD.remove(temp_path);
        job_set_message(job, "Rename failed");
        LOGE("SDJob", "Upload rename failed %s", target_path.c_str());
        return false;
    }

    LOGI("SDJob", "Upload committed %s", target_path.c_str());

    return true;
}

static bool handle_render_next(SdJob *job) {
    if (!job) return false;

    if (!image_render_service_render_next(job->mode, job->last_index, job->last_name)) {
        job_set_message(job, "Render failed");
        return false;
    }

    return true;
}

static void worker_task(void *param) {
    (void)param;
    SdJob *job = nullptr;
    while (true) {
        if (xQueueReceive(g_job_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!job) continue;

        job->state = SdJobState::Running;
        job->updated_ms = millis();
        LOGI("SDJob", "Start job %lu type=%u", (unsigned long)job->id, (unsigned)job->type);

        if (!ensure_sd_ready_internal()) {
            job->state = SdJobState::Error;
            job->success = false;
            job_set_message(job, "SD init failed");
            job->updated_ms = millis();
            LOGE("SDJob", "Job %lu failed: SD init failed", (unsigned long)job->id);
            continue;
        }

        bool ok = false;
        switch (job->type) {
            case SdJobType::List: {
                job->names.clear();
                ok = collect_g4_names(job->names);
                if (ok) {
                    sort_names(job->names);
                } else {
                    job_set_message(job, "SD unavailable");
                }
                break;
            }
            case SdJobType::Delete: {
                if (!is_valid_g4_name(job->name)) {
                    job_set_message(job, "Invalid name");
                    ok = false;
                    break;
                }
                const String path = "/" + String(job->name);
                if (!SD.exists(path)) {
                    job_set_message(job, "Not found");
                    ok = false;
                    break;
                }
                ok = SD.remove(path);
                if (!ok) job_set_message(job, "Delete failed");
                break;
            }
            case SdJobType::Upload: {
                ok = write_upload_to_sd(job);
                break;
            }
            case SdJobType::Display: {
                if (!is_valid_g4_name(job->name)) {
                    job_set_message(job, "Invalid name");
                    ok = false;
                    break;
                }
                const String path = "/" + String(job->name);
                if (!SD.exists(path)) {
                    job_set_message(job, "Not found");
                    ok = false;
                    break;
                }
                if (!it8951_renderer_init()) {
                    job_set_message(job, "Render init failed");
                    ok = false;
                    break;
                }
                ok = it8951_render_g4(path.c_str());
                if (!ok) job_set_message(job, "Render failed");
                break;
            }
            case SdJobType::RenderNext: {
                ok = handle_render_next(job);
                break;
            }
            default:
                job_set_message(job, "Unknown job");
                ok = false;
                break;
        }

        job->success = ok;
        job->state = ok ? SdJobState::Done : SdJobState::Error;
        job->updated_ms = millis();

        if (ok) {
            LOGI("SDJob", "Job %lu done", (unsigned long)job->id);
        } else {
            LOGW("SDJob", "Job %lu error: %s", (unsigned long)job->id, job->message);
        }

        if (job->buffer) {
            heap_caps_free(job->buffer);
            job->buffer = nullptr;
            job->buffer_size = 0;
        }
    }
}

static uint32_t enqueue_job(SdJob *job) {
    if (!job) return 0;
    if (!store_job(job)) {
        free_job(job);
        return 0;
    }
    if (!g_job_queue) {
        free_job(job);
        return 0;
    }
    if (xQueueSend(g_job_queue, &job, 0) != pdTRUE) {
        job->state = SdJobState::Error;
        job->success = false;
        job_set_message(job, "Queue full");
        job->updated_ms = millis();
        LOGW("SDJob", "Queue full for job %lu type=%u", (unsigned long)job->id, (unsigned)job->type);
        return job->id;
    }
    LOGI("SDJob", "Enqueued job %lu type=%u", (unsigned long)job->id, (unsigned)job->type);
    return job->id;
}
}

bool sd_storage_configure(SPIClass &spi, const SdCardPins &pins, uint32_t frequency_hz) {
    g_spi = &spi;
    g_pins = pins;
    g_frequency = frequency_hz;

    if (!g_job_queue) {
        g_job_queue = xQueueCreate(kJobQueueDepth, sizeof(SdJob *));
    }

    if (!g_worker_task) {
        xTaskCreatePinnedToCore(worker_task, "sd_worker", kWorkerStackSize, nullptr, kWorkerPriority, &g_worker_task, kWorkerCore);
    }

    return g_job_queue && g_worker_task;
}

bool sd_storage_ensure_ready() {
    return ensure_sd_ready_internal();
}

uint32_t sd_storage_enqueue_list() {
    SdJob *job = alloc_job();
    if (!job) return 0;
    job->type = SdJobType::List;
    return enqueue_job(job);
}

uint32_t sd_storage_enqueue_delete(const char *name) {
    SdJob *job = alloc_job();
    if (!job) return 0;
    job->type = SdJobType::Delete;
    if (name) {
        strlcpy(job->name, name, sizeof(job->name));
    }
    return enqueue_job(job);
}

uint32_t sd_storage_enqueue_upload(const char *name, uint8_t *buffer, size_t size) {
    SdJob *job = alloc_job();
    if (!job) return 0;
    job->type = SdJobType::Upload;
    if (name) {
        strlcpy(job->name, name, sizeof(job->name));
    }
    job->buffer = buffer;
    job->buffer_size = size;
    return enqueue_job(job);
}

uint32_t sd_storage_enqueue_display(const char *name) {
    SdJob *job = alloc_job();
    if (!job) return 0;
    job->type = SdJobType::Display;
    if (name) {
        strlcpy(job->name, name, sizeof(job->name));
    }
    return enqueue_job(job);
}

uint32_t sd_storage_enqueue_render_next(
    SdImageSelectMode mode,
    uint32_t last_index,
    const char *last_name
) {
    SdJob *job = alloc_job();
    if (!job) return 0;
    job->type = SdJobType::RenderNext;
    job->mode = mode;
    job->last_index = last_index;
    if (last_name) {
        strlcpy(job->last_name, last_name, sizeof(job->last_name));
    }
    return enqueue_job(job);
}

bool sd_storage_get_job(uint32_t id, SdJobInfo *out) {
    if (!out || id == 0) return false;
    SdJob *job = find_job(id);
    if (!job) return false;

    out->id = job->id;
    out->type = job->type;
    out->state = job->state;
    out->success = job->success;
    out->bytes = job->bytes;
    out->created_ms = job->created_ms;
    out->updated_ms = job->updated_ms;
    strlcpy(out->message, job->message, sizeof(out->message));
    return true;
}

bool sd_storage_get_job_names(uint32_t id, std::vector<String> &out_names) {
    SdJob *job = find_job(id);
    if (!job) return false;
    if (job->type != SdJobType::List) return false;
    if (job->state != SdJobState::Done) return false;
    out_names = job->names;
    return true;
}

void sd_storage_purge_jobs() {
    portENTER_CRITICAL(&g_jobs_mux);
    gc_jobs_locked();
    portEXIT_CRITICAL(&g_jobs_mux);
}
