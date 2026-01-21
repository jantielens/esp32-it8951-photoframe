#include "render_scheduler.h"

#include "sd_storage_service.h"
#include "image_render_service.h"
#include "rtc_state.h"
#include "log_manager.h"

namespace {
static uint32_t g_render_job_id = 0;
static uint32_t g_refresh_interval_ms = 0;
static uint32_t g_retry_interval_ms = 0;
static bool g_pending_refresh = true;
static unsigned long g_last_refresh_ms = 0;
static unsigned long g_next_attempt_ms = 0;
static SdImageSelectMode g_mode = SdImageSelectMode::Random;
static RenderPreEnqueueHook g_pre_enqueue_hook = nullptr;
static void *g_pre_enqueue_context = nullptr;

static bool enqueue_render_job() {
    if (g_render_job_id != 0) return false;

    if (g_pre_enqueue_hook) {
        g_pre_enqueue_hook(g_pre_enqueue_context);
    }

    uint32_t working_index = rtc_image_state_get_last_image_index();
    char working_name_buf[RTC_IMAGE_NAME_MAX_LEN] = {0};
    const char *rtc_name = rtc_image_state_get_last_image_name();
    if (rtc_name && rtc_name[0] != '\0') {
        strlcpy(working_name_buf, rtc_name, sizeof(working_name_buf));
    }

    g_render_job_id = sd_storage_enqueue_render_next(g_mode, working_index, working_name_buf);
    if (g_render_job_id == 0) {
        LOGW("Render", "Enqueue render job failed");
        return false;
    }
    LOGI("Render", "Enqueued render job id=%lu mode=%s", (unsigned long)g_render_job_id,
         g_mode == SdImageSelectMode::Sequential ? "sequential" : "random");
    return true;
}

static bool poll_render_job(bool *out_success) {
    if (g_render_job_id == 0) return false;
    SdJobInfo info = {};
    if (!sd_storage_get_job(g_render_job_id, &info)) {
        g_render_job_id = 0;
        if (out_success) *out_success = false;
        return true;
    }

    if (info.state == SdJobState::Done || info.state == SdJobState::Error) {
        if (out_success) *out_success = info.success;
        LOGI("Render", "Job %lu complete state=%s ok=%s",
             (unsigned long)g_render_job_id,
             info.state == SdJobState::Done ? "done" : "error",
             info.success ? "true" : "false");
        g_render_job_id = 0;
        return true;
    }
    return false;
}
}

void render_scheduler_init(const DeviceConfig &config, uint32_t refresh_interval_ms, uint32_t retry_interval_ms) {
    g_render_job_id = 0;
    g_refresh_interval_ms = refresh_interval_ms;
    g_retry_interval_ms = retry_interval_ms;
    g_pending_refresh = true;
    g_last_refresh_ms = 0;
    g_next_attempt_ms = 0;
    g_mode = (strcmp(config.image_selection_mode, "sequential") == 0)
        ? SdImageSelectMode::Sequential
        : SdImageSelectMode::Random;
    LOGI("Render", "Scheduler init mode=%s refresh=%lums retry=%lums",
         g_mode == SdImageSelectMode::Sequential ? "sequential" : "random",
         (unsigned long)g_refresh_interval_ms,
         (unsigned long)g_retry_interval_ms);
}

void render_scheduler_request_refresh() {
    if (!g_pending_refresh) {
        LOGI("Render", "Refresh requested");
    }
    g_pending_refresh = true;
}

void render_scheduler_set_pre_enqueue_hook(RenderPreEnqueueHook hook, void *context) {
    g_pre_enqueue_hook = hook;
    g_pre_enqueue_context = context;
}

void render_scheduler_tick() {
    const unsigned long now = millis();
    bool render_success = false;

    if (poll_render_job(&render_success)) {
        if (render_success) {
            g_last_refresh_ms = now;
            g_pending_refresh = false;
            g_next_attempt_ms = 0;
        } else {
            LOGW("Render", "Job failed; retry in %lums", (unsigned long)g_retry_interval_ms);
            g_next_attempt_ms = now + g_retry_interval_ms;
        }
    }

    if (g_pending_refresh || (g_refresh_interval_ms > 0 && (now - g_last_refresh_ms >= g_refresh_interval_ms))) {
        if (now >= g_next_attempt_ms && g_render_job_id == 0) {
            if (!enqueue_render_job()) {
                LOGW("Render", "Enqueue failed; retry in %lums", (unsigned long)g_retry_interval_ms);
                g_next_attempt_ms = now + g_retry_interval_ms;
            }
        }
    }
}

bool render_scheduler_render_once(
    const DeviceConfig &config,
    SPIClass &spi,
    const SdCardPins &pins,
    uint32_t frequency_hz
) {
    const unsigned long sd_start = millis();
    if (!sd_photo_picker_init(spi, pins, frequency_hz)) {
        LOGE("SD", "Init failed");
        return false;
    }
    LOG_DURATION("SD", "Init", sd_start);

    randomSeed(analogRead(0));

    const SdImageSelectMode mode = (strcmp(config.image_selection_mode, "sequential") == 0)
        ? SdImageSelectMode::Sequential
        : SdImageSelectMode::Random;
    const uint32_t last_index = rtc_image_state_get_last_image_index();
    const char *last_name = rtc_image_state_get_last_image_name();

    return image_render_service_render_next(mode, last_index, last_name);
}
