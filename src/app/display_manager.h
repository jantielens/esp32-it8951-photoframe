#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include "board_config.h"
#include "config_manager.h"
#include "display_driver.h"
#include "eink_ui.h"

#if HAS_IMAGE_API
#include "screens/direct_image_screen.h"
#endif

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
    bool startUi();
    void stopUi();
    bool isUiActive() const { return uiActive; }

    bool showScreen(const char* screen_id);
    const char* getCurrentScreenId();
    const ScreenInfo* getAvailableScreens(size_t* count);

    void setSplashStatus(const char* text);
    void tick();

    DisplayDriver* getDriver() { return driver; }

#if HAS_IMAGE_API
    DirectImageScreen* getDirectImageScreen() { return &directImage; }
    void showDirectImage();
#endif

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

#if HAS_IMAGE_API
    DirectImageScreen directImage;
#endif
};

// Global instance (managed by app.ino)
extern DisplayManager* displayManager;

// C-style interface
void display_manager_init(DeviceConfig* config);
void display_manager_show_splash();
void display_manager_render_now();
void display_manager_render_now_ex(bool full_refresh);
void display_manager_force_full_refresh();
void display_manager_render_full_now();
bool display_manager_ui_start();
void display_manager_ui_stop();
bool display_manager_ui_is_active();
void display_manager_show_screen(const char* screen_id, bool* success);
const char* display_manager_get_current_screen_id();
const ScreenInfo* display_manager_get_available_screens(size_t* count);
void display_manager_set_splash_status(const char* text);
void display_manager_tick();

// Serialization helpers (no-op for the simplified UI)
void display_manager_lock();
void display_manager_unlock();
bool display_manager_try_lock(uint32_t timeout_ms);

// Best-effort perf stats for diagnostics (/api/health).
bool display_manager_get_perf_stats(DisplayPerfStats* out);

#if HAS_IMAGE_API
DirectImageScreen* display_manager_get_direct_image_screen();
void display_manager_show_direct_image();
#endif

#endif // DISPLAY_MANAGER_H
