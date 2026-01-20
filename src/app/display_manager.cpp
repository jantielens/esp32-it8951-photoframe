#include "board_config.h"

#if HAS_DISPLAY

#include "display_manager.h"
#include "log_manager.h"

#include <esp_timer.h>

// Include selected display driver header.
// Driver implementations are compiled via src/app/display_drivers.cpp.
#include "drivers/it8951_lvgl_driver.h"

static portMUX_TYPE g_splash_status_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE g_perf_mux = portMUX_INITIALIZER_UNLOCKED;

static DisplayPerfStats g_perf = {0, 0, 0};
static bool g_perf_ready = false;
static uint32_t g_perf_window_start_ms = 0;
static uint16_t g_perf_frames_in_window = 0;

// Global instance
DisplayManager* displayManager = nullptr;

DisplayManager::DisplayManager(DeviceConfig* cfg) 
        : driver(nullptr), config(cfg), currentScreen(nullptr), pendingScreen(nullptr),
            lvglTaskHandle(nullptr), lvglMutex(nullptr), lvglTaskStopRequested(false),
            screenCount(0), buf(nullptr), lvglDisp(nullptr), flushPending(false),
            forceFullRefreshNext(false),
            lastPresentMs(0),
            uiActive(false), pendingSplashStatusSet(false) {
        pendingSplashStatus[0] = '\0';

        // Instantiate selected display driver
        #if DISPLAY_DRIVER == DISPLAY_DRIVER_IT8951
        driver = new IT8951_LVGL_Driver();
        #else
        #error "No display driver selected or unknown driver type"
        #endif
    
    // Create mutex for thread-safe LVGL access
    lvglMutex = xSemaphoreCreateMutex();
    
    // Initialize screen registry (splash only for now).
    availableScreens[0] = {"splash", "Splash", &splashScreen};
    screenCount = 1;
}

DisplayManager::~DisplayManager() {
    // Stop rendering task
    if (lvglTaskHandle) {
        lvglTaskStopRequested = true;
        uint32_t waitMs = 0;
        while (lvglTaskHandle && waitMs < 2000) {
            vTaskDelay(pdMS_TO_TICKS(10));
            waitMs += 10;
        }
        if (lvglTaskHandle) {
            vTaskDelete(lvglTaskHandle);
            lvglTaskHandle = nullptr;
        }
    }
    
    if (currentScreen) {
        currentScreen->hide();
    }
    
    splashScreen.destroy();
    
    // Delete display driver
    if (driver) {
        delete driver;
        driver = nullptr;
    }
    
    // Delete mutex
    if (lvglMutex) {
        vSemaphoreDelete(lvglMutex);
        lvglMutex = nullptr;
    }
    
    // Free LVGL buffer
    if (buf) {
        heap_caps_free(buf);
        buf = nullptr;
    }
}

const char* DisplayManager::getScreenIdForInstance(const Screen* screen) const {
    if (!screen) return nullptr;

    if (screen == &splashScreen) {
        return "splash";
    }

    // Registered runtime screens.
    for (size_t i = 0; i < screenCount; i++) {
        if (availableScreens[i].instance == screen) {
            return availableScreens[i].id;
        }
    }

    return nullptr;
}

// LVGL flush callback
void DisplayManager::flushCallback(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    DisplayManager* mgr = (DisplayManager*)disp->user_data;

    if (mgr && mgr->driver) {
        mgr->driver->flushArea(area, color_p);
        mgr->flushPending = true;
    }
    
    lv_disp_flush_ready(disp);
}

bool DisplayManager::isInLvglTask() const {
    if (!lvglTaskHandle) return false;
    return xTaskGetCurrentTaskHandle() == lvglTaskHandle;
}

void DisplayManager::lockIfNeeded(bool& didLock) {
    if (isInLvglTask()) {
        didLock = false;
        return;
    }
    lock();
    didLock = true;
}

void DisplayManager::unlockIfNeeded(bool didLock) {
    if (didLock) {
        unlock();
    }
}

void DisplayManager::lock() {
    if (lvglMutex) {
        xSemaphoreTake(lvglMutex, portMAX_DELAY);
    }
}

void DisplayManager::unlock() {
    if (lvglMutex) {
        xSemaphoreGive(lvglMutex);
    }
}

bool DisplayManager::tryLock(uint32_t timeoutMs) {
    if (!lvglMutex) return false;
    return xSemaphoreTake(lvglMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

// FreeRTOS task for continuous LVGL rendering
void DisplayManager::lvglTask(void* pvParameter) {
    DisplayManager* mgr = (DisplayManager*)pvParameter;
    
    LOGI("Display", "LVGL render task start (core %d)", xPortGetCoreID());
    
    while (!mgr->lvglTaskStopRequested) {
        mgr->lock();

        // Apply any deferred splash status update.
        if (mgr->pendingSplashStatusSet) {
            char text[sizeof(mgr->pendingSplashStatus)];
            bool has = false;
            portENTER_CRITICAL(&g_splash_status_mux);
            if (mgr->pendingSplashStatusSet) {
                strlcpy(text, mgr->pendingSplashStatus, sizeof(text));
                mgr->pendingSplashStatusSet = false;
                has = true;
            }
            portEXIT_CRITICAL(&g_splash_status_mux);
            if (has) {
                mgr->splashScreen.setStatus(text);
            }
        }
        
        // Process pending screen switch (deferred from external calls)
        if (mgr->pendingScreen) {
            Screen* target = mgr->pendingScreen;
            if (mgr->currentScreen) {
                mgr->currentScreen->hide();
            }

            mgr->currentScreen = target;
            mgr->currentScreen->show();
            mgr->pendingScreen = nullptr;

            const char* screenId = mgr->getScreenIdForInstance(mgr->currentScreen);
            LOGI("Display", "Switched to %s", screenId ? screenId : "(unregistered)");
        }
        
        if (mgr->forceFullRefreshNext) {
            lv_obj_invalidate(lv_scr_act());
        }

        // Handle LVGL rendering (animations, timers, etc.)
        const uint64_t lv_start_us = esp_timer_get_time();
        uint32_t delayMs = lv_timer_handler();
        const uint32_t lv_timer_us = (uint32_t)(esp_timer_get_time() - lv_start_us);
        
        // Update current screen (data refresh)
        if (mgr->currentScreen) {
            mgr->currentScreen->update();
        }
        
        // Flush canvas buffer only when LVGL produced draw data.
        if (mgr->flushPending) {
            const uint32_t now_ms = millis();
            const uint32_t min_interval = mgr->driver ? mgr->driver->minPresentIntervalMs() : 0;

            if (min_interval == 0 || (now_ms - mgr->lastPresentMs) >= min_interval) {
                if (g_perf_window_start_ms == 0) {
                    g_perf_window_start_ms = now_ms;
                    g_perf_frames_in_window = 0;
                }

                uint64_t present_start_us = 0;
                if (mgr->driver && mgr->driver->renderMode() == DisplayDriver::RenderMode::Buffered) {
                    present_start_us = esp_timer_get_time();
                    mgr->driver->present(mgr->forceFullRefreshNext);
                }

                const uint32_t present_us = (present_start_us == 0) ? 0 : (uint32_t)(esp_timer_get_time() - present_start_us);
                g_perf_frames_in_window++;

                // Update published stats every ~1s.
                const uint32_t elapsed = now_ms - g_perf_window_start_ms;
                if (elapsed >= 1000) {
                    const uint16_t fps = g_perf_frames_in_window;
                    portENTER_CRITICAL(&g_perf_mux);
                    g_perf.fps = fps;
                    g_perf.lv_timer_us = lv_timer_us;
                    g_perf.present_us = present_us;
                    g_perf_ready = true;
                    portEXIT_CRITICAL(&g_perf_mux);

                    g_perf_window_start_ms = now_ms;
                    g_perf_frames_in_window = 0;
                }

                mgr->lastPresentMs = now_ms;
                mgr->forceFullRefreshNext = false;
                mgr->flushPending = false;
            }
        }
        
        mgr->unlock();
        
        // Sleep based on LVGL's suggested next timer deadline.
        // Clamp to keep UI responsive while avoiding busy looping on static screens.
        if (delayMs < 1) delayMs = 1;
        if (delayMs > 20) delayMs = 20;
        vTaskDelay(pdMS_TO_TICKS(delayMs));
    }

    mgr->lvglTaskStopRequested = false;
    mgr->lvglTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

bool display_manager_get_perf_stats(DisplayPerfStats* out) {
    if (!out) return false;
    bool ok = false;
    portENTER_CRITICAL(&g_perf_mux);
    ok = g_perf_ready;
    if (ok) {
        *out = g_perf;
    }
    portEXIT_CRITICAL(&g_perf_mux);
    return ok;
}

void DisplayManager::initHardware() {
    LOGI("Display", "Init start");
    
    // Initialize display driver
    driver->init();
    
    LOGI("Display", "Resolution: %dx%d", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    LOGI("Display", "Rotation: %d", DISPLAY_ROTATION);

    LOGI("Display", "Init complete");
}

void DisplayManager::initLVGL() {
    LOGI("Display", "LVGL init start");
    
    lv_init();
    
    // Allocate LVGL draw buffer.
    // Some QSPI panels/drivers require internal RAM for flush reliability.
    if (LVGL_BUFFER_PREFER_INTERNAL) {
        buf = (lv_color_t*)heap_caps_malloc(LVGL_BUFFER_SIZE * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!buf) {
            LOGW("Display", "Internal RAM alloc failed, trying PSRAM...");
            buf = (lv_color_t*)heap_caps_malloc(LVGL_BUFFER_SIZE * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
        }
    } else {
        // Default: PSRAM first, fallback to internal.
        buf = (lv_color_t*)heap_caps_malloc(LVGL_BUFFER_SIZE * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
        if (!buf) {
            LOGW("Display", "PSRAM alloc failed, trying internal RAM...");
            buf = (lv_color_t*)heap_caps_malloc(LVGL_BUFFER_SIZE * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
    }
    if (!buf) {
        LOGE("Display", "Failed to allocate LVGL buffer");
        return;
    }
    LOGI("Display", "Buffer allocated: %d bytes (%d pixels)", LVGL_BUFFER_SIZE * sizeof(lv_color_t), LVGL_BUFFER_SIZE);
    
    // Initialize default theme (dark mode with custom primary color)
    lv_theme_t* theme = lv_theme_default_init(
        NULL,                           // Display (use default)
        lv_color_hex(0x3399FF),        // Primary color (light blue)
        lv_color_hex(0x303030),        // Secondary color (dark gray)
        true,                           // Dark mode
        LV_FONT_DEFAULT                // Default font
    );
    lv_disp_set_theme(NULL, theme);
    LOGI("Display", "Theme: Default dark mode initialized");
    
    // Set up display buffer
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, LVGL_BUFFER_SIZE);
    
    // Initialize display driver
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = DISPLAY_WIDTH;
    disp_drv.ver_res = DISPLAY_HEIGHT;
    disp_drv.flush_cb = DisplayManager::flushCallback;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.user_data = this;  // Pass instance for callback
    
    // Apply driver-specific LVGL configuration (rotation, full refresh, etc.)
    driver->configureLVGL(&disp_drv);
    
    lvglDisp = lv_disp_drv_register(&disp_drv);
    if (!lvglDisp) {
        lvglDisp = lv_disp_get_default();
    }
    
    LOGI("Display", "Buffer: %d pixels (%d lines)", LVGL_BUFFER_SIZE, LVGL_BUFFER_SIZE / DISPLAY_WIDTH);
    LOGI("Display", "LVGL init complete");
}

void DisplayManager::init() {
    // Initialize hardware (IT8951)
    initHardware();
    
    // Initialize LVGL
    initLVGL();
    
    LOGI("Display", "Manager init start");
    
    // Create all screens
    splashScreen.create();
    
    LOGI("Display", "Screens created");
    
    // Show splash immediately
    showSplash();

    // Deterministic mode: do not start LVGL task by default.
    uiActive = false;

    LOGI("Display", "Manager init complete");
}

void DisplayManager::renderNow() {
    bool didLock = false;
    lockIfNeeded(didLock);

    // Apply any deferred splash status update.
    if (pendingSplashStatusSet) {
        char text[sizeof(pendingSplashStatus)];
        bool has = false;
        portENTER_CRITICAL(&g_splash_status_mux);
        if (pendingSplashStatusSet) {
            strlcpy(text, pendingSplashStatus, sizeof(text));
            pendingSplashStatusSet = false;
            has = true;
        }
        portEXIT_CRITICAL(&g_splash_status_mux);
        if (has) {
            splashScreen.setStatus(text);
        }
    }

    // Process any pending screen switch.
    if (pendingScreen) {
        Screen* target = pendingScreen;
        if (currentScreen) {
            currentScreen->hide();
        }
        currentScreen = target;
        currentScreen->show();
        pendingScreen = nullptr;

        const char* screenId = getScreenIdForInstance(currentScreen);
        LOGI("Display", "Switched to %s", screenId ? screenId : "(unregistered)");
    }

    if (forceFullRefreshNext) {
        lv_obj_invalidate(lv_scr_act());
    }

    // Force LVGL to render immediately.
    lv_timer_handler();

    // Present immediately if LVGL produced draw data.
    if (flushPending) {
        if (driver && driver->renderMode() == DisplayDriver::RenderMode::Buffered) {
            driver->present(forceFullRefreshNext);
        }
        flushPending = false;
        forceFullRefreshNext = false;
    }

    unlockIfNeeded(didLock);
}

void DisplayManager::renderFullNow() {
    bool didLock = false;
    lockIfNeeded(didLock);

    // Ensure LVGL redraws full screen into the framebuffer.
    forceFullRefreshNext = true;
    lv_obj_invalidate(lv_scr_act());
    lv_timer_handler();

    if (flushPending) {
        if (driver && driver->renderMode() == DisplayDriver::RenderMode::Buffered) {
            driver->present(true);
        }
        flushPending = false;
        forceFullRefreshNext = false;
    }

    unlockIfNeeded(didLock);
}

bool DisplayManager::startUi() {
    if (uiActive) return true;
    if (!driver) {
        LOGE("Display", "startUi: no display driver");
        return false;
    }

    if (lvglTaskHandle) {
        uiActive = true;
        return true;
    }

    lvglTaskStopRequested = false;
    #if CONFIG_FREERTOS_UNICORE
    xTaskCreate(lvglTask, "LVGL", 8192, this, 1, &lvglTaskHandle);
    LOGI("Display", "UI start: rendering task created (single-core)");
    #else
    xTaskCreatePinnedToCore(lvglTask, "LVGL", 8192, this, 1, &lvglTaskHandle, 0);
    LOGI("Display", "UI start: rendering task created (pinned to Core 0)");
    #endif
    uiActive = true;
    return true;
}

void DisplayManager::stopUi() {
    if (!uiActive) return;
    uiActive = false;

    if (!lvglTaskHandle) return;

    lvglTaskStopRequested = true;
    if (isInLvglTask()) {
        return;
    }

    uint32_t waitMs = 0;
    while (lvglTaskHandle && waitMs < 2000) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waitMs += 10;
    }
    if (lvglTaskHandle) {
        LOGW("Display", "stopUi: timeout waiting for LVGL task to stop");
    }
}

void DisplayManager::showSplash() {
    // Splash shown during init - can switch immediately (no task running yet)
    lock();
    if (currentScreen) {
        currentScreen->hide();
    }
    currentScreen = &splashScreen;
    currentScreen->show();
    forceFullRefreshNext = true;
    unlock();
    LOGI("Display", "Switched to SplashScreen");
}

void DisplayManager::forceFullRefresh() {
    bool didLock = false;
    lockIfNeeded(didLock);
    forceFullRefreshNext = true;
    unlockIfNeeded(didLock);
}


void DisplayManager::setSplashStatus(const char* text) {
    // If called before the LVGL task exists (during early setup), update directly.
    // Otherwise, defer to the LVGL task to avoid cross-task LVGL calls.
    if (!lvglTaskHandle || isInLvglTask()) {
        bool didLock = false;
        lockIfNeeded(didLock);
        splashScreen.setStatus(text);
        unlockIfNeeded(didLock);
        return;
    }

    portENTER_CRITICAL(&g_splash_status_mux);
    strlcpy(pendingSplashStatus, text ? text : "", sizeof(pendingSplashStatus));
    pendingSplashStatusSet = true;
    portEXIT_CRITICAL(&g_splash_status_mux);
}

bool DisplayManager::showScreen(const char* screen_id) {
    if (!screen_id) return false;
    
    // Look up screen in registry
    for (size_t i = 0; i < screenCount; i++) {
        if (strcmp(availableScreens[i].id, screen_id) == 0) {
            // Defer screen switch to lvglTask (non-blocking)
            pendingScreen = availableScreens[i].instance;
            LOGI("Display", "Queued switch to screen: %s", screen_id);
            return true;
        }
    }
    
    LOGW("Display", "Screen not found: %s", screen_id);
    return false;
}

const char* DisplayManager::getCurrentScreenId() {
    // Return ID of current screen (nullptr if splash or unknown)
    for (size_t i = 0; i < screenCount; i++) {
        if (currentScreen == availableScreens[i].instance) {
            return availableScreens[i].id;
        }
    }
    return nullptr;  // Splash or unknown screen
}

const ScreenInfo* DisplayManager::getAvailableScreens(size_t* count) {
    if (count) *count = screenCount;
    return availableScreens;
}

// C-style interface for app.ino
void display_manager_init(DeviceConfig* config) {
    if (!displayManager) {
        displayManager = new DisplayManager(config);
        displayManager->init();
    }
}

void display_manager_show_splash() {
    if (displayManager) {
        displayManager->showSplash();
    }
}

void display_manager_render_now() {
    if (displayManager) {
        displayManager->renderNow();
    }
}

void display_manager_render_full_now() {
    if (displayManager) {
        displayManager->renderFullNow();
    }
}

void display_manager_force_full_refresh() {
    if (displayManager) {
        displayManager->forceFullRefresh();
    }
}

bool display_manager_ui_start() {
    if (!displayManager) return false;
    return displayManager->startUi();
}

void display_manager_ui_stop() {
    if (displayManager) {
        displayManager->stopUi();
    }
}

bool display_manager_ui_is_active() {
    if (!displayManager) return false;
    return displayManager->isUiActive();
}

void display_manager_set_splash_status(const char* text) {
    if (displayManager) {
        displayManager->setSplashStatus(text);
    }
}

void display_manager_show_screen(const char* screen_id, bool* success) {
    bool result = false;
    if (displayManager) {
        result = displayManager->showScreen(screen_id);
    }
    if (success) *success = result;
}

const char* display_manager_get_current_screen_id() {
    if (displayManager) {
        return displayManager->getCurrentScreenId();
    }
    return nullptr;
}

const ScreenInfo* display_manager_get_available_screens(size_t* count) {
    if (displayManager) {
        return displayManager->getAvailableScreens(count);
    }
    if (count) *count = 0;
    return nullptr;
}


void display_manager_lock() {
    if (displayManager) {
        displayManager->lock();
    }
}

void display_manager_unlock() {
    if (displayManager) {
        displayManager->unlock();
    }
}

bool display_manager_try_lock(uint32_t timeout_ms) {
    if (!displayManager) return false;
    return displayManager->tryLock(timeout_ms);
}


#endif // HAS_DISPLAY
