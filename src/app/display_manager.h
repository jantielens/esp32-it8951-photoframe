#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include "board_config.h"
#include "config_manager.h"
#include "display_driver.h"
#include "eink_ui.h"


// ============================================================================
// Screen Registry (logical UI modes)
// ============================================================================
struct ScreenInfo {
    const char* id;
    const char* display_name;
};

// Lightweight rendering/perf snapshot (best-effort).
struct DisplayPerfStats {
    uint16_t fps;
    uint32_t lv_timer_us;
    uint32_t present_us;
};

class DisplayManager {
public:
    explicit DisplayManager(DeviceConfig* config);
    ~DisplayManager();

    void init();
    void showSplash();
    void renderNow();
    void renderNow(bool fullRefresh);
    void renderFullNow();
    void forceFullRefresh();
    void stopUi();
    bool isUiActive() const { return uiActive; }

    bool showScreen(const char* screen_id);
    const char* getCurrentScreenId();
    const ScreenInfo* getAvailableScreens(size_t* count);

    void setSplashStatus(const char* text);
    void tick();

    DisplayDriver* getDriver() { return driver; }

private:
    void initHardware();

    DisplayDriver* driver;
    DeviceConfig* config;
    EInkUi ui;

    bool uiActive;
    bool forceFullRefreshNext;
    uint32_t lastPresentMs;
    uint8_t splashPartialCount;

    const char* currentScreenId;
    ScreenInfo availableScreens[2];
    size_t screenCount;

};

// Global instance (managed by app.ino)
extern DisplayManager* displayManager;

// C-style interface
void display_manager_init(DeviceConfig* config);
void display_manager_render_now();
void display_manager_render_now_ex(bool full_refresh);
void display_manager_force_full_refresh();
void display_manager_render_full_now();
void display_manager_ui_stop();
bool display_manager_ui_is_active();
void display_manager_show_screen(const char* screen_id, bool* success);
const char* display_manager_get_current_screen_id();
const ScreenInfo* display_manager_get_available_screens(size_t* count);
void display_manager_set_splash_status(const char* text);
void display_manager_tick();

// Best-effort perf stats for diagnostics (/api/health).
bool display_manager_get_perf_stats(DisplayPerfStats* out);

#endif // DISPLAY_MANAGER_H
