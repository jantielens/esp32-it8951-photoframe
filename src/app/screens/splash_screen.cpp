
#include "screens/splash_screen.h"
#include "log_manager.h"

static void layoutSplashBlock(lv_obj_t* screen, lv_obj_t* titleLabel, lv_obj_t* statusLabel) {
    if (!screen || !titleLabel || !statusLabel) return;

    // Ensure LVGL has calculated object sizes before we query heights
    lv_obj_update_layout(screen);
    lv_obj_update_layout(titleLabel);
    lv_obj_update_layout(statusLabel);

    const int gap_title_to_status = 16;

    const int screen_h = (int)lv_obj_get_height(screen);
    const int title_h = (int)lv_obj_get_height(titleLabel);
    const int status_h = (int)lv_obj_get_height(statusLabel);

    const int block_h = title_h + gap_title_to_status + status_h;
    int top = (screen_h - block_h) / 2;
    if (top < 0) top = 0;

    lv_obj_align(titleLabel, LV_ALIGN_TOP_MID, 0, top);
    lv_obj_align_to(statusLabel, titleLabel, LV_ALIGN_OUT_BOTTOM_MID, 0, gap_title_to_status);
}

SplashScreen::SplashScreen() : screen(nullptr), titleLabel(nullptr), statusLabel(nullptr) {}

SplashScreen::~SplashScreen() {
    destroy();
}

void SplashScreen::create() {
    LOGI("Splash", "Create start");
    if (screen) {
        LOGI("Splash", "Already created");
        return;  // Already created
    }
    
    // Create screen
    screen = lv_obj_create(NULL);
    // Override theme background to white (e-ink friendly)
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);

    // Title text
    titleLabel = lv_label_create(screen);
    lv_label_set_text(titleLabel, "Photo Frame");
    lv_obj_set_style_text_align(titleLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(titleLabel, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(titleLabel, lv_color_black(), 0);

    // Status text
    statusLabel = lv_label_create(screen);
    lv_label_set_text(statusLabel, "Booting...");
    lv_label_set_long_mode(statusLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(statusLabel, (lv_coord_t)(lv_obj_get_width(screen) - 24));
    lv_obj_set_style_text_align(statusLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(statusLabel, lv_color_black(), 0);

    // Position the whole block.
    layoutSplashBlock(screen, titleLabel, statusLabel);
    
    LOGI("Splash", "Screen created");
}

void SplashScreen::destroy() {
    if (screen) {
        lv_obj_del(screen);
        screen = nullptr;
        titleLabel = nullptr;
        statusLabel = nullptr;
    }
}

void SplashScreen::show() {
    if (screen) {
        lv_scr_load(screen);
    }
}

void SplashScreen::hide() {
    // Nothing to do - LVGL handles screen switching
}

void SplashScreen::update() {
    // Static screen - no updates needed
}

void SplashScreen::setStatus(const char* text) {
    if (statusLabel) {
        LOGI("Splash", "Status: %s", text ? text : "(null)");
        lv_label_set_text(statusLabel, text);

        // Re-layout in case the text height changed (wrapping, font changes, etc.)
        layoutSplashBlock(screen, titleLabel, statusLabel);
    } else {
        LOGE("Splash", "statusLabel is NULL");
    }
}
