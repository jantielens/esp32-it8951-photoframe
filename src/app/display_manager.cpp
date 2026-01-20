#include "board_config.h"

#if HAS_DISPLAY

#include "display_manager.h"
#include "log_manager.h"
#include "project_branding.h"
#include "drivers/it8951_display_driver.h"

// Global instance
DisplayManager* displayManager = nullptr;

DisplayManager::DisplayManager(DeviceConfig* cfg)
    : driver(nullptr), config(cfg), uiActive(false), forceFullRefreshNext(false),
      lastPresentMs(0), splashPartialCount(0), currentScreenId(nullptr), screenCount(0)
{
    #if DISPLAY_DRIVER == DISPLAY_DRIVER_IT8951
    driver = new IT8951_Display_Driver();
    #else
    #error "No display driver selected or unknown driver type"
    #endif

    availableScreens[0] = {"splash", "Splash"};
    screenCount = 1;

}

DisplayManager::~DisplayManager() {
    if (driver) {
        delete driver;
        driver = nullptr;
    }
}

void DisplayManager::initHardware() {
    LOGI("Display", "Init start");
    driver->init();
    LOGI("Display", "Resolution: %dx%d", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    LOGI("Display", "Rotation: %d", DISPLAY_ROTATION);
    LOGI("Display", "Init complete");
}

void DisplayManager::init() {
    initHardware();

    if (!ui.init(driver)) {
        LOGE("Display", "UI init failed");
    }
    ui.setTitle(PROJECT_DISPLAY_NAME);
    ui.setStatus("Booting...");

    showSplash();
    uiActive = false;

    LOGI("Display", "Manager init complete");
}

void DisplayManager::showSplash() {
    currentScreenId = "splash";
}

void DisplayManager::renderNow() {
    renderNow(false);
}

void DisplayManager::renderNow(bool fullRefresh) {
    if (!driver || driver->isBusy()) return;

    const uint32_t now_ms = millis();
    const uint32_t min_interval = driver->minPresentIntervalMs();
    if (min_interval != 0 && (uint32_t)(now_ms - lastPresentMs) < min_interval) {
        return;
    }

    const bool is_splash = currentScreenId && strcmp(currentScreenId, "splash") == 0;
    const uint8_t kSplashPartialMax = 5;
    bool do_full = fullRefresh || forceFullRefreshNext;
    if (is_splash && splashPartialCount >= kSplashPartialMax) {
        do_full = true;
    }

    ui.render(do_full, is_splash && !do_full);
    lastPresentMs = now_ms;
    forceFullRefreshNext = false;

    if (is_splash) {
        if (do_full) {
            splashPartialCount = 0;
        } else if (ui.didPartialLast()) {
            splashPartialCount++;
        } else {
            splashPartialCount = 0;
        }
    }
}

void DisplayManager::renderFullNow() {
    renderNow(true);
}

void DisplayManager::forceFullRefresh() {
    forceFullRefreshNext = true;
}

void DisplayManager::stopUi() {
    uiActive = false;
}

bool DisplayManager::showScreen(const char* screen_id) {
    if (!screen_id) return false;
    if (strcmp(screen_id, "splash") == 0) {
        showSplash();
        return true;
    }
    return false;
}

const char* DisplayManager::getCurrentScreenId() {
    return currentScreenId;
}

const ScreenInfo* DisplayManager::getAvailableScreens(size_t* count) {
    if (count) *count = screenCount;
    return availableScreens;
}

void DisplayManager::setSplashStatus(const char* text) {
    ui.setStatus(text);
}

void DisplayManager::tick() {
}

// C-style interface
void display_manager_init(DeviceConfig* config) {
    if (!displayManager) {
        displayManager = new DisplayManager(config);
        displayManager->init();
    }
}

void display_manager_render_now() {
    if (displayManager) {
        displayManager->renderNow();
    }
}

void display_manager_render_now_ex(bool full_refresh) {
    if (displayManager) {
        displayManager->renderNow(full_refresh);
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

void display_manager_tick() {
    if (displayManager) {
        displayManager->tick();
    }
}

bool display_manager_get_perf_stats(DisplayPerfStats* out) {
    (void)out;
    return false;
}

#endif // HAS_DISPLAY
