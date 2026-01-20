#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include "board_config.h"
#include "config_manager.h"
#include "display_driver.h"
#include "screens/screen.h"
#include "screens/splash_screen.h"

#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// ============================================================================
// Screen Registry
// ============================================================================
#define MAX_SCREENS 4

// Struct for registering available screens dynamically
struct ScreenInfo {
    const char* id;            // Unique identifier (e.g., "splash")
    const char* display_name;  // Human-readable name
    Screen* instance;          // Pointer to screen instance
};

// ============================================================================
// Display Manager
// ============================================================================
// Manages display hardware, LVGL, screen lifecycle, and navigation.
// Uses FreeRTOS task for continuous LVGL rendering (works on single and dual core).
//
// Usage:
//   display_manager_init(&device_config);   // In setup() - starts rendering task
//   display_manager_show_splash();          // Show splash screen
//   display_manager_set_splash_status();    // Update splash text
//
// Note: No need to call update() in loop() - rendering task handles it

class DisplayManager {
private:
    // Hardware (display driver abstraction)
    DisplayDriver* driver;
    lv_disp_draw_buf_t draw_buf;
    lv_color_t* buf;  // Dynamically allocated LVGL buffer
    lv_disp_drv_t disp_drv;
    lv_disp_t* lvglDisp;
    
    // Configuration reference
    DeviceConfig* config;
    
    // FreeRTOS task and mutex
    TaskHandle_t lvglTaskHandle;
    SemaphoreHandle_t lvglMutex;
    volatile bool lvglTaskStopRequested;
    
    // Screen management
    Screen* currentScreen;
    Screen* pendingScreen;   // Deferred screen switch (processed in lvglTask)

    // UI session state (LVGL task running)
    bool uiActive;

    // Defer small LVGL UI updates (like splash status) to the LVGL task.
    char pendingSplashStatus[96];
    volatile bool pendingSplashStatusSet;

    // Helpers: avoid taking the LVGL mutex when already inside the LVGL task
    bool isInLvglTask() const;
    void lockIfNeeded(bool& didLock);
    void unlockIfNeeded(bool didLock);
    
    // Screen instances (created at init, kept in memory)
    SplashScreen splashScreen;
    
    // Screen registry for runtime navigation (static allocation, no heap)
    ScreenInfo availableScreens[MAX_SCREENS];
    size_t screenCount;

    // Internal helper: map a Screen instance to its logical screen id.
    // Uses the registered screen list so adding new screens doesn't require
    // updating logging code.
    const char* getScreenIdForInstance(const Screen* screen) const;
    
    // Hardware initialization
    void initHardware();
    void initLVGL();
    
    // LVGL flush callback (static, accesses instance via user_data)
    static void flushCallback(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p);

    // Buffered render-mode drivers need an explicit present() step,
    // but only after LVGL has actually rendered something.
    bool flushPending;

    // Force a full refresh on the next present() (clears ghosting).
    bool forceFullRefreshNext;

    // Last present timestamp for throttling (e-ink panels).
    uint32_t lastPresentMs;
    
    // FreeRTOS task for LVGL rendering
    static void lvglTask(void* pvParameter);
    
public:
    DisplayManager(DeviceConfig* config);
    ~DisplayManager();
    
    // Initialize hardware + LVGL + screens + rendering task (shows splash automatically)
    void init();
    
    // Navigation API (thread-safe)
    void showSplash();

    // Deterministic render: refresh LVGL and present immediately (blocking)
    void renderNow();

    // Deterministic full refresh: redraw full screen and present with full waveform.
    void renderFullNow();

    // Request a full refresh on the next render/present.
    void forceFullRefresh();

    // UI session control
    bool startUi();
    void stopUi();
    bool isUiActive() const { return uiActive; }
    
    // Screen selection by ID (thread-safe, returns true if found)
    bool showScreen(const char* screen_id);
    
    // Get current screen ID (returns nullptr if splash or no screen)
    const char* getCurrentScreenId();
    
    // Get available screens for runtime navigation
    const ScreenInfo* getAvailableScreens(size_t* count);
    
    // Splash status update (thread-safe)
    void setSplashStatus(const char* text);
    
    // Mutex helpers for external thread-safe access
    void lock();
    void unlock();

    // Attempt to lock the LVGL mutex with a timeout (in milliseconds).
    // Returns true if the lock was acquired.
    bool tryLock(uint32_t timeoutMs);

    // Active LVGL logical resolution (post driver->configureLVGL()).
    // Prefer using these instead of calling LVGL APIs from non-LVGL tasks.
    int getActiveWidth() const { return (int)disp_drv.hor_res; }
    int getActiveHeight() const { return (int)disp_drv.ver_res; }
    
    // Access to splash screen for status updates
    SplashScreen* getSplash() { return &splashScreen; }
    
    // Access to display driver (for touch integration)
    DisplayDriver* getDriver() { return driver; }
};

// Lightweight rendering/perf snapshot (best-effort).
struct DisplayPerfStats {
    uint16_t fps;
    uint32_t lv_timer_us;
    uint32_t present_us;
};

// Global instance (managed by app.ino)
extern DisplayManager* displayManager;

// C-style interface for app.ino
void display_manager_init(DeviceConfig* config);
void display_manager_show_splash();
void display_manager_render_now();
void display_manager_force_full_refresh();
void display_manager_render_full_now();
bool display_manager_ui_start();
void display_manager_ui_stop();
bool display_manager_ui_is_active();
void display_manager_show_screen(const char* screen_id, bool* success);  // success is optional output
const char* display_manager_get_current_screen_id();
const ScreenInfo* display_manager_get_available_screens(size_t* count);
void display_manager_set_splash_status(const char* text);

// Serialization helpers for code running outside the LVGL task.
void display_manager_lock();
void display_manager_unlock();
bool display_manager_try_lock(uint32_t timeout_ms);

// Best-effort perf stats for diagnostics (/api/health).
// Returns false until a first stats window has been captured.
bool display_manager_get_perf_stats(DisplayPerfStats* out);

#endif // DISPLAY_MANAGER_H
