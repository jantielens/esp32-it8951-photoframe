#include "sd_storage_service.h"

#include "log_manager.h"
#include "rtc_state.h"
#include "it8951_renderer.h"
#include "image_render_service.h"
#include "display_manager.h"
#include "web_portal_render_control.h"
#include "azure_blob_client.h"
#include "config_manager.h"
#include "time_utils.h"

#include <SD.h>
#include <vector>
#include <algorithm>

#include <WiFi.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/portmacro.h>

#include <esp_heap_caps.h>

namespace {
static constexpr size_t kMaxJobs = 16;
static constexpr size_t kMaxNameLen = 127;
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

    char sas_url[CONFIG_BLOB_SAS_URL_MAX_LEN] = {0};

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

static bool has_prefix(const char *name, const char *prefix) {
    if (!name || !prefix) return false;
    const size_t prefix_len = strlen(prefix);
    return strncmp(name, prefix, prefix_len) == 0;
}

static bool is_valid_g4_name(const char *name) {
    if (!name) return false;
    const size_t len = strlen(name);
    if (len == 0 || len > kMaxNameLen) return false;
    if (strchr(name, '\\')) return false;
    if (strstr(name, "..")) return false;
    if (len < 3 || strcmp(name + (len - 3), ".g4") != 0) return false;

    size_t slash_count = 0;
    for (const char *p = name; *p; ++p) {
        if (*p == '/') slash_count++;
    }

    if (slash_count == 0) return true;
    if (slash_count == 1 && (has_prefix(name, "queue-permanent/") || has_prefix(name, "queue-temporary/"))) return true;
    return false;
}

static bool parse_all_temp_expiry(const String &name, time_t *out_epoch) {
    if (!out_epoch) return false;
    if (!name.startsWith("all/temporary/")) return false;
    const int prefix_len = strlen("all/temporary/");
    const int first_sep = name.indexOf("__", prefix_len);
    if (first_sep < 0) return false;
    const String ts = name.substring(prefix_len, first_sep);
    return time_utils::parse_utc_timestamp(ts.c_str(), out_epoch);
}

static bool derive_queue_name_from_all_blob(const String &all_name, String &out_queue_name) {
    if (all_name.startsWith("all/temporary/")) {
        out_queue_name = String("queue-temporary/") + all_name.substring(strlen("all/temporary/"));
        return true;
    }
    if (all_name.startsWith("all/permanent/")) {
        out_queue_name = String("queue-permanent/") + all_name.substring(strlen("all/permanent/"));
        return true;
    }
    return false;
}

static bool collect_g4_names_from_dir(const char *dir, const char *prefix, std::vector<String> &names) {
    if (!dir) return false;
    if (!SD.exists(dir)) return true;

    File root = SD.open(dir);
    if (!root) return false;
    if (!root.isDirectory()) {
        root.close();
        return false;
    }

    File file = root.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            const char *name = file.name();
            if (name && name[0] != '\0') {
                const size_t len = strlen(name);
                if (len >= 3 && strcmp(name + (len - 3), ".g4") == 0) {
                    String entry = prefix ? String(prefix) + String(name) : String(name);
                    if (entry.length() <= kMaxNameLen) {
                        names.push_back(entry);
                    }
                }
            }
        }
        file.close();
        file = root.openNextFile();
    }

    root.close();
    return true;
}

static bool collect_g4_names(std::vector<String> &names) {
    bool ok = true;
    ok = collect_g4_names_from_dir("/queue-permanent", "queue-permanent/", names) && ok;
    ok = collect_g4_names_from_dir("/queue-temporary", "queue-temporary/", names) && ok;
    return ok;
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

    const int last_slash = target_path.lastIndexOf('/');
    if (last_slash > 0) {
        const String dir = target_path.substring(0, last_slash);
        if (!SD.exists(dir)) {
            if (!SD.mkdir(dir)) {
                job_set_message(job, "Create dir failed");
                LOGE("SDJob", "Upload mkdir failed %s", dir.c_str());
                return false;
            }
        }
    }

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

static bool delete_all_g4_files(SdJob *job) {
    std::vector<String> names;
    if (!collect_g4_names(names)) {
        job_set_message(job, "SD unavailable");
        return false;
    }

    size_t deleted = 0;
    for (const auto &name : names) {
        const String path = "/" + name;
        if (SD.exists(path)) {
            if (SD.remove(path)) {
                deleted++;
            } else {
                LOGW("SDJob", "Failed deleting %s", path.c_str());
            }
        }
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "Deleted %u files", (unsigned)deleted);
    job_set_message(job, buf);
    return true;
}

static bool list_all_g4_blobs(
    SdJob *job,
    const AzureSasUrlParts &sas,
    const String &prefix,
    std::vector<String> &out
) {
    out.clear();
    String marker;
    String next_marker;
    std::vector<String> names;

    while (true) {
        names.clear();
        next_marker = "";
        const bool ok = azure_blob_list_page(
            sas,
            prefix,
            marker,
            200,
            names,
            next_marker,
            10000,
            2,
            150
        );
        if (!ok) {
            job_set_message(job, "Azure list failed");
            return false;
        }

        for (const auto &n : names) {
            if (n.endsWith(".g4")) {
                out.push_back(n);
            }
        }

        if (next_marker.length() == 0) break;
        marker = next_marker;
    }

    sort_names(out);
    return true;
}

static bool handle_sync_from_azure(SdJob *job) {
    if (!job) return false;
    if (!job->sas_url[0]) {
        job_set_message(job, "Missing SAS URL");
        return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
        job_set_message(job, "WiFi not connected");
        return false;
    }

    AzureSasUrlParts sas;
    if (!azure_blob_parse_sas_url(job->sas_url, sas)) {
        job_set_message(job, "Invalid SAS URL");
        return false;
    }

    LOGI("SDJob", "SyncFromAzure start");

    const bool was_paused = web_portal_render_is_paused();
    web_portal_render_set_paused(true);

    job_set_message(job, "Deleting SD files...");
    if (!delete_all_g4_files(job)) {
        web_portal_render_set_paused(was_paused);
        return false;
    }

    if (!time_utils::is_time_valid()) {
        job_set_message(job, "Time not synced");
        web_portal_render_set_paused(was_paused);
        return false;
    }

    std::vector<String> queue_temp_blobs;
    std::vector<String> queue_perm_blobs;
    job_set_message(job, "Listing Azure queue-temporary/...");
    if (!list_all_g4_blobs(job, sas, "queue-temporary/", queue_temp_blobs)) {
        web_portal_render_set_paused(was_paused);
        return false;
    }

    job_set_message(job, "Listing Azure queue-permanent/...");
    if (!list_all_g4_blobs(job, sas, "queue-permanent/", queue_perm_blobs)) {
        web_portal_render_set_paused(was_paused);
        return false;
    }

    std::vector<String> all_temp_blobs;
    std::vector<String> all_perm_blobs;
    job_set_message(job, "Listing Azure all/temporary/...");
    if (!list_all_g4_blobs(job, sas, "all/temporary/", all_temp_blobs)) {
        web_portal_render_set_paused(was_paused);
        return false;
    }

    job_set_message(job, "Listing Azure all/permanent/...");
    if (!list_all_g4_blobs(job, sas, "all/permanent/", all_perm_blobs)) {
        web_portal_render_set_paused(was_paused);
        return false;
    }

    LOGI("SDJob", "SyncFromAzure listed queue-temp=%u queue-perm=%u all-temp=%u all-perm=%u",
        (unsigned)queue_temp_blobs.size(),
        (unsigned)queue_perm_blobs.size(),
        (unsigned)all_temp_blobs.size(),
        (unsigned)all_perm_blobs.size());

    std::vector<String> queue_names;
    queue_names.reserve(queue_temp_blobs.size() + queue_perm_blobs.size());
    queue_names.insert(queue_names.end(), queue_temp_blobs.begin(), queue_temp_blobs.end());
    queue_names.insert(queue_names.end(), queue_perm_blobs.begin(), queue_perm_blobs.end());
    sort_names(queue_names);

    struct SyncTarget {
        String blob_name;
        String queue_name;
        bool is_temp;
    };

    std::vector<SyncTarget> targets;
    targets.reserve(all_temp_blobs.size() + all_perm_blobs.size());

    const time_t now = time(nullptr);
    auto is_queued = [&](const String &name) -> bool {
        return std::binary_search(queue_names.begin(), queue_names.end(), name,
            [](const String &a, const String &b) { return a.compareTo(b) < 0; });
    };

    auto add_target = [&](const String &all_name, bool is_temp) {
        String queue_name;
        if (!derive_queue_name_from_all_blob(all_name, queue_name)) {
            LOGW("SDJob", "SyncFromAzure skip invalid all name: %s", all_name.c_str());
            return;
        }
        if (is_queued(queue_name)) {
            return;
        }
        if (is_temp) {
            time_t expiry = 0;
            if (parse_all_temp_expiry(all_name, &expiry) && now >= expiry) {
                return;
            }
        }
        targets.push_back({all_name, queue_name, is_temp});
    };

    for (const auto &b : all_temp_blobs) add_target(b, true);
    for (const auto &b : all_perm_blobs) add_target(b, false);

    const size_t total = targets.size();
    size_t ok_count = 0;
    size_t fail_count = 0;
    size_t idx = 0;
    job->names.clear();

    auto download_and_write = [&](const SyncTarget &target) {
        idx++;
        char msg[96];
        snprintf(msg, sizeof(msg), "Downloading %u/%u...", (unsigned)idx, (unsigned)total);
        job_set_message(job, msg);

        uint8_t *buf = nullptr;
        size_t size = 0;
        int http_code = 0;
        const bool ok_dl = azure_blob_download_to_buffer_ex(
            sas,
            target.blob_name,
            &buf,
            &size,
            15000,
            2,
            150,
            &http_code
        );
        if (!ok_dl || !buf || size == 0) {
            LOGW("SDJob", "SyncFromAzure download failed: %s (http=%d)", target.blob_name.c_str(), http_code);
            fail_count++;
            job->names.push_back(target.blob_name);
            if (buf) heap_caps_free(buf);
            return;
        }

        // Reuse upload write path (write under queue-temporary/ or queue-permanent/).
        strlcpy(job->name, target.queue_name.c_str(), sizeof(job->name));
        job->buffer = buf;
        job->buffer_size = size;
        const bool ok_write = write_upload_to_sd(job);
        if (!ok_write) {
            LOGW("SDJob", "SyncFromAzure write failed: %s", target.queue_name.c_str());
            fail_count++;
            job->names.push_back(target.queue_name);
        } else {
            ok_count++;
        }

        if (job->buffer) {
            heap_caps_free(job->buffer);
            job->buffer = nullptr;
            job->buffer_size = 0;
        }
        job->name[0] = '\0';
    };

    for (const auto &t : targets) download_and_write(t);

    char final_msg[96];
    snprintf(final_msg, sizeof(final_msg), "Synced: ok=%u failed=%u", (unsigned)ok_count, (unsigned)fail_count);
    job_set_message(job, final_msg);

    web_portal_render_set_paused(was_paused);
    LOGI("SDJob", "SyncFromAzure done ok=%u failed=%u", (unsigned)ok_count, (unsigned)fail_count);
    return fail_count == 0;
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
                const bool ui_was_active = display_manager_ui_is_active();
                if (ui_was_active) {
                    display_manager_ui_stop();
                }
                ok = it8951_render_g4(path.c_str());
                if (!ok) job_set_message(job, "Render failed");
                break;
            }
            case SdJobType::RenderNext: {
                ok = handle_render_next(job);
                break;
            }
            case SdJobType::SyncFromAzure: {
                ok = handle_sync_from_azure(job);
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

uint32_t sd_storage_enqueue_sync_from_azure(const char *container_sas_url) {
    SdJob *job = alloc_job();
    if (!job) return 0;
    job->type = SdJobType::SyncFromAzure;
    if (container_sas_url) {
        strlcpy(job->sas_url, container_sas_url, sizeof(job->sas_url));
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
    if (job->type != SdJobType::List && job->type != SdJobType::SyncFromAzure) return false;
    if (job->state != SdJobState::Done) return false;
    out_names = job->names;
    return true;
}

void sd_storage_purge_jobs() {
    portENTER_CRITICAL(&g_jobs_mux);
    gc_jobs_locked();
    portEXIT_CRITICAL(&g_jobs_mux);
}
