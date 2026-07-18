#include <inttypes.h>
#include <stdio.h>

#include "lv_conf.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ui.h"
#include "ui_ipc.h"
#include "ui_helpers.h"
#include "global_state.h"
#include "system.h"
#include "macros.h"
#include "button.h"

#include "nvs_config.h"
#include "displayDriver.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

static const char *TAG = "TDisplayS3";

// ---------- pixel-scaler state (initialised in initTDisplayS3) ----------
// File-scope so the static lvglFlushCallback can access them without extra indirection.
static lv_color_t *s_scale_buf    = nullptr; // DMA-capable output buffer for scaled flush
static int         s_scale_buf_px = 0;       // allocated size in pixels
static int         s_src_w        = 320;     // LVGL logical width  (source space)
static int         s_src_h        = 170;     // LVGL logical height (source space)
static int         s_dst_w        = 320;     // Physical LCD width  (dest space)
static int         s_dst_y_off    = 0;       // LCD y offset to centre scaled UI in LCD height
static bool        s_scale_active = false;   // false = pass-through (T-Display S3)

#ifdef NERDQX
#define SPLASH1_TIMEOUT_MS 3000
#define SPLASH2_TIMEOUT_MS 5000
#else
#define SPLASH1_TIMEOUT_MS 3000
#define SPLASH2_TIMEOUT_MS 3000
#endif

// small helpers
static inline int64_t now_us() { return esp_timer_get_time(); }
static inline int32_t elapsed_ms(int64_t start_us, int64_t now) {
    return static_cast<int32_t>((now - start_us) / 1000);
}

static void formatHashrate(char *buf, int len, float hashrate) {
    if (hashrate >= 10000.0) {
        snprintf(buf, len, "%d", (int) (hashrate + 0.5f));
    } else {
        snprintf(buf, len, "%.1f", hashrate);
    }
}

DisplayDriver::DisplayDriver() {
    m_animationsEnabled = false;
    m_lastKeypressTime = 0;
    m_displayIsOn = false;
    m_countdownActive = false;
    m_countdownStartTime = 0;
    m_btcPrice = 0;
    m_blockHeight = 0;
    m_isActiveOverlay = false;
    m_lvglMutex = PTHREAD_MUTEX_INITIALIZER;
    m_isAutoScreenOffEnabled = false;
    m_tempControlMode = 0;
    m_fanSpeed = 0;
    m_shutdownCountdownActive = false;
    m_shutdownStartTime = 0;
    m_shutdownLabel = nullptr;
    m_buttonIgnoreUntil_us = 0;
}

void DisplayDriver::loadSettings() {
    PThreadGuard lock(m_lvglMutex);
    m_isAutoScreenOffEnabled = Config::isAutoScreenOffEnabled();
    m_tempControlMode = Config::getTempControlMode();
    m_fanSpeed = Config::getFanSpeed();
    m_showFoundBlockEnabled = Config::isShowBlockFoundEnabled();

    // when setting was changed, turn on the display LED
    if (!m_isAutoScreenOffEnabled) {
        displayTurnOn();
    }
}

bool DisplayDriver::notifyLvglFlushReady(esp_lcd_panel_io_handle_t panelIo, esp_lcd_panel_io_event_data_t* edata,
                                   void* userCtx) {
    lv_disp_drv_t* dispDriver = (lv_disp_drv_t*)userCtx;
    lv_disp_flush_ready(dispDriver);
    return false;
}

void DisplayDriver::lvglFlushCallback(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* colorMap) {
    esp_lcd_panel_handle_t panelHandle = (esp_lcd_panel_handle_t)drv->user_data;

    if (!s_scale_active) {
        // No scaling needed — direct pass-through (T-Display S3 path)
        esp_lcd_panel_draw_bitmap(panelHandle, area->x1, area->y1, area->x2 + 1, area->y2 + 1, colorMap);
        return;
    }

    // Nearest-neighbour upscale: LVGL 320×170 source → s_dst_w×(170*scale) LCD dest.
    // Both axes use the same isotropic scale factor (= s_dst_w / s_src_w = 1.5 for 480-wide panel).
    const int src_x1 = area->x1, src_x2 = area->x2;
    const int src_y1 = area->y1, src_y2 = area->y2;
    const int src_w  = src_x2 - src_x1 + 1;
    const int src_h  = src_y2 - src_y1 + 1;

    // Map dirty rect from LVGL space to LCD space (integer floor arithmetic)
    const int dst_x1 =  src_x1      * s_dst_w / s_src_w;
    const int dst_x2 = (src_x2 + 1) * s_dst_w / s_src_w - 1;
    const int dst_y1 =  src_y1      * s_dst_w / s_src_w + s_dst_y_off;
    const int dst_y2 = (src_y2 + 1) * s_dst_w / s_src_w - 1 + s_dst_y_off;
    const int out_w  = dst_x2 - dst_x1 + 1;
    const int out_h  = dst_y2 - dst_y1 + 1;

    // Safety guard — should never fire; falls back to unscaled draw if buffer is too small
    if (out_w <= 0 || out_h <= 0 || out_w * out_h > s_scale_buf_px) {
        ESP_LOGE("scale", "scaler overflow: out=%dx%d buf=%d", out_w, out_h, s_scale_buf_px);
        esp_lcd_panel_draw_bitmap(panelHandle, area->x1, area->y1, area->x2 + 1, area->y2 + 1, colorMap);
        return;
    }

    // Precompute x-source LUT for this dirty rect — avoids a divide in the inner loop
    static int sx_lut[480]; // static: only one display, LVGL flush is single-threaded
    for (int dx = 0; dx < out_w; dx++) {
        int sx = dx * src_w / out_w;
        sx_lut[dx] = (sx < src_w) ? sx : src_w - 1;
    }

    // Nearest-neighbour fill
    lv_color_t *out = s_scale_buf;
    for (int dy = 0; dy < out_h; dy++) {
        int sy = dy * src_h / out_h;
        if (sy >= src_h) sy = src_h - 1;
        const lv_color_t *src_row = colorMap + (size_t)sy * src_w;
        for (int dx = 0; dx < out_w; dx++) {
            *out++ = src_row[sx_lut[dx]];
        }
    }

    esp_lcd_panel_draw_bitmap(panelHandle, dst_x1, dst_y1, dst_x2 + 1, dst_y2 + 1, s_scale_buf);
}

/************ DISPLAY TURN ON/OFF FUNCTIONS *************/
bool DisplayDriver::displayTurnOff(void) {
    if (!m_displayIsOn) {
        return false;
    }
    gpio_set_level(TDISPLAYS3_PIN_NUM_BK_LIGHT, TDISPLAYS3_LCD_BK_LIGHT_OFF_LEVEL);
    gpio_set_level(TDISPLAYS3_PIN_PWR, false);
    ESP_LOGI(TAG, "Screen off");
    m_displayIsOn = false;
    return true;
}

bool DisplayDriver::displayTurnOn(void) {
    if (m_displayIsOn) {
        return false;
    }
    gpio_set_level(TDISPLAYS3_PIN_PWR, true);
    gpio_set_level(TDISPLAYS3_PIN_NUM_BK_LIGHT, TDISPLAYS3_LCD_BK_LIGHT_ON_LEVEL);
    ESP_LOGI(TAG, "Screen on");
    m_displayIsOn = true;
    return true;
}

/************ AUTO TURN OFF DISPLAY FUNCTIONS *************/
void DisplayDriver::startCountdown(void) {
    m_countdownActive = true;
    m_countdownStartTime = esp_timer_get_time();

    if (m_countdownLabel == NULL) {
        lv_obj_t* currentScreen = lv_scr_act();
        lv_obj_t* blackBox = lv_obj_create(currentScreen);
        lv_obj_set_size(blackBox, 200, 100);
        lv_obj_set_style_bg_color(blackBox, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_border_width(blackBox, 0, LV_PART_MAIN);
        lv_obj_align(blackBox, LV_ALIGN_CENTER, 0, 0);

        m_countdownLabel = lv_label_create(blackBox);
        lv_label_set_text(m_countdownLabel, "Turning screen off...");
        lv_obj_set_style_text_color(m_countdownLabel, lv_color_white(), LV_PART_MAIN);
        lv_obj_center(m_countdownLabel);
    }
}

void DisplayDriver::displayHideCountdown(void) {
    if (m_countdownLabel) {
        lv_obj_del(lv_obj_get_parent(m_countdownLabel));
        m_countdownLabel = NULL;
    }
}

void DisplayDriver::checkAutoTurnOffScreen(void) {
    if (!m_displayIsOn)
        return;

    int64_t currentTime = esp_timer_get_time();

    if ((currentTime - m_lastKeypressTime) > 30000000) {  // 30 seconds timeout
        if (!m_countdownActive) {
            startCountdown();
        }

        int64_t elapsedTime = (currentTime - m_countdownStartTime) / 1000000;  // Convert to seconds

        if (elapsedTime > 5) {
            displayHideCountdown();
            displayTurnOff();
            m_countdownActive = false;
        }

    } else {
        if (m_countdownActive) {
            displayHideCountdown();
            m_countdownActive = false;
        }
    }
}

void DisplayDriver::startShutdownCountdown() {
    if (m_shutdownCountdownActive) return;

    m_shutdownCountdownActive = true;
    m_isActiveOverlay = true;
    m_shutdownStartTime = esp_timer_get_time();

    lv_obj_t* scr = lv_scr_act();
    lv_obj_t* box = lv_obj_create(scr);
    lv_obj_set_size(box, 200, 100);
    lv_obj_set_style_bg_color(box, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 0, LV_PART_MAIN);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);

    m_shutdownLabel = lv_label_create(box);
    lv_label_set_text(m_shutdownLabel, "Shutdown in 5");
    lv_obj_set_style_text_color(m_shutdownLabel, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(m_shutdownLabel);
}

void DisplayDriver::updateShutdownCountdown() {
    if (!m_shutdownCountdownActive) return;

    int elapsed = (esp_timer_get_time() - m_shutdownStartTime) / 1000000;
    int remaining = 5 - elapsed;
    if (remaining < 0) remaining = 0;

    if (m_shutdownLabel) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Shutdown in %d", remaining);
        lv_label_set_text(m_shutdownLabel, buf);
    }

    // After 5s → trigger shutdown
    if (elapsed >= 5) {
        hideShutdownCountdown();
        enterState(UiState::PowerOff, esp_timer_get_time());
    }
}

void DisplayDriver::hideShutdownCountdown() {
    if (m_shutdownLabel) {
        lv_obj_del(lv_obj_get_parent(m_shutdownLabel));
        m_shutdownLabel = nullptr;
    }
    m_shutdownCountdownActive = false;
    m_isActiveOverlay = false;
}


void DisplayDriver::increaseLvglTick() {
    lv_tick_inc(TDISPLAYS3_LVGL_TICK_PERIOD_MS);
}

// Refresh screen values
void DisplayDriver::refreshScreen(void) {
    // NOP
}

void DisplayDriver::showError(const char *error_message, uint32_t error_code) {
    PThreadGuard lock(m_lvglMutex);
    // hide the overlay and free the memory in case it was open
    m_ui->hideErrorOverlay();

    // now show the (new) error overlay
    m_ui->showErrorOverlay(error_message, error_code);
    m_isActiveOverlay = true;
    refreshScreen();
}

void DisplayDriver::hideError() {
    PThreadGuard lock(m_lvglMutex);
    // hide the overlay and free the memory
    m_ui->hideErrorOverlay();
    m_isActiveOverlay = false;
}

void DisplayDriver::showFoundBlockOverlay() {
    PThreadGuard lock(m_lvglMutex);
    // hide the overlay and free the memory in case it was open
    m_ui->hideImageOverlay();

    // not enabled?
    if (!m_showFoundBlockEnabled) {
        return;
    }

    // now show the (new) image overlay
    m_ui->showImageOverlay(&ui_img_found_block_png);
    m_isActiveOverlay = true;
    refreshScreen();
}

void DisplayDriver::hideFoundBlockOverlay() {
    // hide the overlay and free the memory
    m_ui->hideImageOverlay();
    m_isActiveOverlay = false;
}

void DisplayDriver::lvglTimerTaskWrapper(void *param) {
    DisplayDriver *display = (DisplayDriver*) param;
    display->lvglTimerTask(NULL);
}

void DisplayDriver::safe_screen_change(lv_obj_t * new_scr, lv_scr_load_anim_t anim_type, uint32_t speed, uint32_t delay)
{
    // Scaling for larger displays (e.g. NerdOCTAXE-Gamma 480×320) is handled in
    // lvglFlushCallback via nearest-neighbour pixel scaling.  No LVGL transform needed.
    m_screenAnimationRunning = true;
    _ui_screen_change(new_scr, anim_type, speed, delay);
}

bool DisplayDriver::enterState(UiState s, int64_t now)
{
    // we already are in this state
    if (m_state == s) {
        return true;
    }
    UiState previousState = m_state;

    m_state = s;
    m_stateStart_us = now;

    switch (m_state) {
    case UiState::NOP:
        // NOP
        break;
    case UiState::Splash1:
        ESP_LOGI(TAG, "enter state splash1");
        enableLvglAnimations(true);
        break;

    case UiState::Splash2:
        ESP_LOGI(TAG, "enter state splash2");
        enableLvglAnimations(true);
        safe_screen_change(m_ui->ui_Splash2, LV_SCR_LOAD_ANIM_FADE_ON, 500, 0);
        if (m_ui->ui_Splash1) { lv_obj_clean(m_ui->ui_Splash1); m_ui->ui_Splash1 = NULL; }
        break;

    case UiState::Wait:
        ESP_LOGI(TAG, "enter state wait");
        if (m_ui->ui_Splash2) { lv_obj_clean(m_ui->ui_Splash2); m_ui->ui_Splash2 = NULL; }
        break;

    case UiState::Portal:
        ESP_LOGI(TAG, "enter state portal");
        enableLvglAnimations(true);
        safe_screen_change(m_ui->ui_PortalScreen, LV_SCR_LOAD_ANIM_FADE_ON, 500, 0);
        break;

    case UiState::Mining:
        ESP_LOGI(TAG, "enter state mining");
        enableLvglAnimations(true);
        if (previousState == UiState::GlobalStats) {
            safe_screen_change(m_ui->ui_MiningScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 350, 0);
        } else {
            safe_screen_change(m_ui->ui_MiningScreen, LV_SCR_LOAD_ANIM_FADE_ON, 500, 0);
        }
        break;

    case UiState::SettingsScreen:
        ESP_LOGI(TAG, "enter state settings screen");
        enableLvglAnimations(true);
        safe_screen_change(m_ui->ui_SettingsScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 350, 0);
        break;

    case UiState::BTCScreen:
        ESP_LOGI(TAG, "enter state btc screen");
        enableLvglAnimations(true);
        safe_screen_change(m_ui->ui_BTCScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 350, 0);
        break;

    case UiState::GlobalStats:
        ESP_LOGI(TAG, "enter state global stats");
        enableLvglAnimations(true);
        safe_screen_change(m_ui->ui_GlobalStats, LV_SCR_LOAD_ANIM_MOVE_LEFT, 350, 0);
        break;
    case UiState::ShowQR:
        ESP_LOGI(TAG, "enter qr state");
        enableLvglAnimations(true);
        safe_screen_change(m_ui->ui_qrScreen, LV_SCR_LOAD_ANIM_FADE_ON, 500, 0);
        break;
    case UiState::PowerOff:
        if (!m_ui->ui_PowerOffScreen) {
            m_ui->powerOffScreenInit();
        }
        enableLvglAnimations(false);
        safe_screen_change(m_ui->ui_PowerOffScreen, LV_SCR_LOAD_ANIM_NONE, 0, 0);
        POWER_MANAGEMENT_MODULE.shutdown();
        break;
    }
    return true;
}


void DisplayDriver::updateState(int64_t now, bool btn1Press, bool btn2Press, bool btnBothLongPress)
{
    const int ms = elapsed_ms(m_stateStart_us, now);

    if (btnBothLongPress) {
        enterState(UiState::PowerOff, now);
        return;
    }

    switch (m_state) {
    case UiState::NOP:
        // NOP
        break;
    case UiState::Splash1:
        if (ms >= SPLASH1_TIMEOUT_MS) {
            enterState(UiState::Splash2, now);
        }
        break;

    case UiState::Splash2:
        if (ms >= SPLASH2_TIMEOUT_MS) {
            enterState(UiState::Wait, now);
        }
        break;

    case UiState::Wait:
        // NOP
        break;

    case UiState::Portal:
        enterState(UiState::Portal, now);
        break;

    case UiState::Mining:
        if (ledControl(btn1Press, btn2Press)) {
            break;
        }
        if (btn1Press) {
            APIs_FETCHER.enableFetching();
            enterState(UiState::SettingsScreen, now);
        } else {
            enterState(UiState::Mining, now);
        }
        break;
    case UiState::SettingsScreen:
        if (ledControl(btn1Press, btn2Press)) {
            break;
        }
        if (btn1Press) {
            enterState(UiState::BTCScreen, now);
            APIs_FETCHER.enableFetching();
        }
        break;
    case UiState::BTCScreen:
        if (ledControl(btn1Press, btn2Press)) {
            break;
        }
        if (btn1Press) {
            enterState(UiState::GlobalStats, now);
        }
        break;
    case UiState::GlobalStats:
        if (ledControl(btn1Press, btn2Press)) {
            break;
        }
        if (btn1Press) {
            enterState(UiState::Mining, now);
        }
        break;
    case UiState::ShowQR:
        if (btn1Press || btn2Press) {
            // abort enrollment
            otp.disableEnrollment();
            enterState(UiState::Mining, now);
        }
        break;
    case UiState::PowerOff:
        // NOP
        break;
    }

}



void DisplayDriver::waitForSplashs() {
    // wait until state is not Splash1 or Splash2
    while ((int) m_state < (int) UiState::Wait) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

bool DisplayDriver::ledControl(bool btn1, bool btn2) {
    // btn1 turns it on
    if (btn1) {
        return displayTurnOn();
    }

    // btn2 toggles the LED
    if (btn2) {
        if (!m_displayIsOn) {
            return displayTurnOn();
        }
        return displayTurnOff();
    }
    return false;
}

uint32_t DisplayDriver::handleLvglTick(int32_t &elapsed_Ani_cycles)
{
    uint32_t wait_ms = 0;

    {
        PThreadGuard lock(m_lvglMutex);
        increaseLvglTick();
        wait_ms = lv_timer_handler();
    }

    if (m_animationsEnabled) {
        const uint32_t fast_cap = 5; // ~200 FPS
        if (++elapsed_Ani_cycles > 80) {
            m_animationsEnabled = false;
            elapsed_Ani_cycles = 0;
        }
        uint32_t sleep_ms = std::min(wait_ms, fast_cap);
        vTaskDelay(pdMS_TO_TICKS(sleep_ms));
        return sleep_ms;
    }

    const uint32_t idle_cap = 50;
    uint32_t sleep_ms = (wait_ms > 0 && wait_ms < idle_cap) ? wait_ms : idle_cap;
    vTaskDelay(pdMS_TO_TICKS(sleep_ms));
    return sleep_ms;
}

void DisplayDriver::processButtons(Button &btn1, Button &btn2, int64_t tnow,
                                   bool &btn1Press, bool &btn2Press, bool &btnBothLongPress)
{
    btn1.update();
    btn2.update();

    uint32_t evt1 = btn1.getEvent();
    uint32_t evt2 = btn2.getEvent();
    bool bothPressed = (evt1 & BTN_EVENT_PRESSED) && (evt2 & BTN_EVENT_PRESSED);
    bool anyPressed = (evt1 & BTN_EVENT_PRESSED) || (evt2 & BTN_EVENT_PRESSED);

    if (anyPressed) {
        m_lastKeypressTime = tnow;
    }

    // Ignore all button events within 200ms of both released
    if (esp_timer_get_time() < m_buttonIgnoreUntil_us) {
        btn1.clearEvent();
        btn2.clearEvent();
        return;
    }

    // Hide overlay if active
    if ((evt1 & BTN_EVENT_SHORTPRESS || evt2 & BTN_EVENT_SHORTPRESS) && m_isActiveOverlay) {
        hideFoundBlockOverlay();
        btn1.clearEvent();
        btn2.clearEvent();
        return;
    }

    // --- Shutdown countdown handling ---
    if (bothPressed) {
        if (!m_shutdownCountdownActive) {
            startShutdownCountdown();
        } else {
            updateShutdownCountdown();
        }
        return;
    }

    if (m_shutdownCountdownActive && !bothPressed) {
        hideShutdownCountdown();
        m_buttonIgnoreUntil_us = esp_timer_get_time() + 200 * 1000; // 200ms ignore
        btn1.clearEvent();
        btn2.clearEvent();
        return;
    }

    // Normal button events
    if ((evt1 & BTN_EVENT_LONGPRESS) && (evt2 & BTN_EVENT_LONGPRESS)) {
        btnBothLongPress = true;
        btn1.clearEvent();
        btn2.clearEvent();
    } else {
        if (evt1 & BTN_EVENT_SHORTPRESS) {
            m_lastKeypressTime = tnow;
            btn1Press = true;
            btn1.clearEvent();
        }
        if (evt2 & BTN_EVENT_SHORTPRESS) {
            m_lastKeypressTime = tnow;
            btn2Press = true;
            btn2.clearEvent();
        }
    }
}

void DisplayDriver::handleUiQueueMessages(ui_msg_t &msg, int64_t tnow)
{
    if (xQueueReceive(g_ui_queue, &msg, 0) != pdTRUE) return;

    switch (msg.type) {
        case UI_CMD_SHOW_QR: {
            if (!otp.isEnrollmentActive()) {
                ESP_LOGE(TAG, "no otp enrollment active");
                break;
            }
            int size = 0;
            uint8_t* qrBuf = otp.getQrCode(&size);
            m_ui->createQRScreen(qrBuf, size);
            if (m_ui->ui_qrScreen)
                enterState(UiState::ShowQR, tnow);
            m_isActiveOverlay = true;
            break;
        }
        case UI_CMD_HIDE_QR:
            m_isActiveOverlay = false;
            enterState(UiState::Mining, tnow);
            break;
    }

    if (msg.payload) {
        free(msg.payload);
        msg.payload = nullptr;
    }
}

void DisplayDriver::handleAutoOffAndOverlays()
{
    if (m_isActiveOverlay) {
        displayTurnOn();
    } else if (m_isAutoScreenOffEnabled) {
        checkAutoTurnOffScreen();
    }
}

void DisplayDriver::lvglTimerTask(void *param)
{
    displayTurnOn();
    m_lastKeypressTime = now_us();
    enterState(UiState::Splash1, now_us());

    int32_t elapsed_Ani_cycles = 0;
    Button btn1(PIN_BUTTON_1, 5000);
    Button btn2(PIN_BUTTON_2, 5000);
    ui_msg_t msg;

    while (true) {
        const int64_t tnow = now_us();
        uint32_t wait_ms = handleLvglTick(elapsed_Ani_cycles);

        if (POWER_MANAGEMENT_MODULE.isShutdown()) {
            // switch into poweroff state
            enterState(UiState::PowerOff, tnow);
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
            continue;
        }

        // --- Handle buttons ---
        bool btn1Press = false, btn2Press = false, btnBothLongPress = false;
        processButtons(btn1, btn2, tnow, btn1Press, btn2Press, btnBothLongPress);

        // animation is running, ignore buttons
        if (m_screenAnimationRunning) {
            btn1Press = false;
            btn2Press = false;
            btnBothLongPress = false;
        }

        // --- Handle queued UI messages ---
        handleUiQueueMessages(msg, tnow);

        // --- Handle auto turn-off and overlays ---
        handleAutoOffAndOverlays();

        // --- Update FSM state ---
        updateState(tnow, btn1Press, btn2Press, btnBothLongPress);
    }
}


// Función para activar las actualizaciones
void DisplayDriver::enableLvglAnimations(bool enable)
{
    m_animationsEnabled = enable;
}

void DisplayDriver::mainCreatSysteTasks(void)
{
    xTaskCreatePinnedToCore(lvglTimerTaskWrapper, "lvgl Timer", 6000, (void*) this, 4, NULL, 1); // Antes 10000
}

lv_obj_t *DisplayDriver::initTDisplayS3(void)
{
    static lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer(s) called draw buffer(s)
    static lv_disp_drv_t disp_drv;      // contains callback functions

    // Resolve board-specific physical display parameters.
    // Board pointer is set before initSystem() → initTDisplayS3() is called, so this is safe.
    Board *board = SYSTEM_MODULE.getBoard();
    int lcd_width        = board->getLCDWidth();        // physical pixels wide  (e.g. 480 for Gamma)
    int lcd_height       = board->getLCDHeight();       // physical pixels tall  (e.g. 320 for Gamma)
    uint32_t lcd_pclk_hz = board->getLCDPixelClockHz(); // i80 pixel clock

    // LVGL always renders at 320×170 — the UI's native design resolution.
    // For displays wider than 320px, lvglFlushCallback scales pixels to fill the panel.
    static const int lvgl_w = 320;
    static const int lvgl_h = 170;
    int lvgl_buf_pixels = (lvgl_w * lvgl_h) / 6; // ~1/6 frame = 9066 px

    // The i80 bus max_transfer_bytes must cover the LARGEST single DMA write we'll ever
    // make — which is a scaled flush rect (up to lvgl_buf * scale²).
    // scale = lcd_width / 320 (e.g. 1.5 for 480-wide panel → factor 2.25, +25% margin)
    float sf = (float)lcd_width / (float)lvgl_w;
    int bus_max_pixels = (int)(lvgl_buf_pixels * sf * sf * 1.25f) + 512;
    size_t bus_max_bytes = ((size_t)(bus_max_pixels * sizeof(uint16_t)) + 63) & ~63UL; // DMA-align

    ESP_LOGI(TAG, "LCD %dx%d pclk=%lu  LVGL %dx%d  buf=%d  bus_max=%u",
             lcd_width, lcd_height, (unsigned long)lcd_pclk_hz,
             lvgl_w, lvgl_h, lvgl_buf_pixels, (unsigned)bus_max_bytes);

    ESP_LOGI(TAG, "Turn off LCD backlight");
    gpio_config_t bk_gpio_config = {.pin_bit_mask = 1ULL << TDISPLAYS3_PIN_NUM_BK_LIGHT, .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));

    gpio_pad_select_gpio(TDISPLAYS3_PIN_NUM_BK_LIGHT);
    gpio_pad_select_gpio(TDISPLAYS3_PIN_RD);
    gpio_pad_select_gpio(TDISPLAYS3_PIN_PWR);
    // esp_rom_gpio_pad_select_gpio(TDISPLAYS3_PIN_NUM_BK_LIGHT);
    // esp_rom_gpio_pad_select_gpio(TDISPLAYS3_PIN_RD);
    // esp_rom_gpio_pad_select_gpio(TDISPLAYS3_PIN_PWR);

    gpio_set_direction(TDISPLAYS3_PIN_NUM_BK_LIGHT, GPIO_MODE_OUTPUT);
    gpio_set_direction(TDISPLAYS3_PIN_RD, GPIO_MODE_OUTPUT);
    gpio_set_direction(TDISPLAYS3_PIN_PWR, GPIO_MODE_OUTPUT);

    gpio_set_level(TDISPLAYS3_PIN_RD, true);
    gpio_set_level(TDISPLAYS3_PIN_NUM_BK_LIGHT, TDISPLAYS3_LCD_BK_LIGHT_OFF_LEVEL);

    ESP_LOGI(TAG, "Initialize Intel 8080 bus");
    esp_lcd_i80_bus_handle_t i80_bus = NULL;
    esp_lcd_i80_bus_config_t bus_config = {.dc_gpio_num = TDISPLAYS3_PIN_NUM_DC,
                                           .wr_gpio_num = TDISPLAYS3_PIN_NUM_PCLK,
                                           .clk_src = LCD_CLK_SRC_DEFAULT,
                                           .data_gpio_nums =
                                               {
                                                   TDISPLAYS3_PIN_NUM_DATA0,
                                                   TDISPLAYS3_PIN_NUM_DATA1,
                                                   TDISPLAYS3_PIN_NUM_DATA2,
                                                   TDISPLAYS3_PIN_NUM_DATA3,
                                                   TDISPLAYS3_PIN_NUM_DATA4,
                                                   TDISPLAYS3_PIN_NUM_DATA5,
                                                   TDISPLAYS3_PIN_NUM_DATA6,
                                                   TDISPLAYS3_PIN_NUM_DATA7,
                                               },
                                           .bus_width = 8,
                                           .max_transfer_bytes = bus_max_bytes,
                                           .psram_trans_align = LCD_PSRAM_TRANS_ALIGN,
                                           .sram_trans_align = LCD_SRAM_TRANS_ALIGN};
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_config, &i80_bus));
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i80_config_t io_config = {
        .cs_gpio_num = TDISPLAYS3_PIN_NUM_CS,
        .pclk_hz = lcd_pclk_hz,
        .trans_queue_depth = 20,
        .on_color_trans_done = notifyLvglFlushReady,
        .user_ctx = &disp_drv,
        .lcd_cmd_bits = TDISPLAYS3_LCD_CMD_BITS,
        .lcd_param_bits = TDISPLAYS3_LCD_PARAM_BITS,
        .dc_levels =
            {
                .dc_idle_level = 0,
                .dc_cmd_level = 0,
                .dc_dummy_level = 0,
                .dc_data_level = 1,
            }
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_bus, &io_config, &io_handle));

    ESP_LOGI(TAG, "Install LCD driver of st7789");
    esp_lcd_panel_handle_t panel_handle = NULL;

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = TDISPLAYS3_PIN_NUM_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

    esp_lcd_panel_reset(panel_handle);
    esp_lcd_panel_init(panel_handle);
    esp_lcd_panel_invert_color(panel_handle, true);

    esp_lcd_panel_swap_xy(panel_handle, true);

    if (!board->isFlipScreenEnabled()) {
        esp_lcd_panel_mirror(panel_handle, true, false);   // MADCTL 0x60 (MV+MX) — standard T-Display S3
    } else {
        esp_lcd_panel_mirror(panel_handle, false, false);  // MADCTL 0x20 (MV only) — last landscape option for NerdOCTAXE-Gamma
    }

    // Gap is board/display specific. T-Display S3 needs y_gap=35 to center 170px in 240px GRAM.
    // NerdOCTAXE-Gamma has a full 240px display — no gap needed (vendor confirmed set_gap(0,0)).
    esp_lcd_panel_set_gap(panel_handle, 0, board->getLCDYGap());

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_LOGI(TAG, "Turn on LCD backlight");
    gpio_set_level(TDISPLAYS3_PIN_PWR, true);
    gpio_set_level(TDISPLAYS3_PIN_NUM_BK_LIGHT, TDISPLAYS3_LCD_BK_LIGHT_ON_LEVEL);

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();
    // alloc draw buffers used by LVGL
    // it's recommended to choose the size of the draw buffer(s) to be at least 1/10 screen sized
    // LVGL buffer: sized for the 320×170 logical canvas (not the physical LCD)
    lv_color_t *buf1 = (lv_color_t*) MALLOC_DMA(lvgl_buf_pixels * sizeof(lv_color_t));
    assert(buf1);
    lv_disp_draw_buf_init(&disp_buf, buf1, NULL, lvgl_buf_pixels);

    ESP_LOGI(TAG, "Register display driver to LVGL");
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = lvgl_w;   // 320 — LVGL logical width  (physical upscaled in flush CB)
    disp_drv.ver_res = lvgl_h;   // 170 — LVGL logical height
    disp_drv.flush_cb = lvglFlushCallback;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;
    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);

    // ---------- Pixel scaler init ----------
    // Populate the file-scope scaler state used by the static lvglFlushCallback.
    s_src_w = lvgl_w;  // 320
    s_src_h = lvgl_h;  // 170
    s_dst_w = lcd_width;
    {
        // Scaled UI height: 170 * (lcd_width/320). For 480-wide: 170*1.5 = 255.
        int dst_h_scaled = lvgl_h * lcd_width / lvgl_w;
        s_dst_y_off      = (lcd_height - dst_h_scaled) / 2; // centre vertically (32 for 480×320)
    }
    s_scale_active = (lcd_width != lvgl_w);

    if (s_scale_active) {
        s_scale_buf_px = bus_max_pixels; // upper-bound pixel count pre-computed above
        s_scale_buf    = (lv_color_t*) MALLOC_DMA(s_scale_buf_px * sizeof(lv_color_t));
        assert(s_scale_buf);
        ESP_LOGI(TAG, "Pixel scaler: %dx%d → %dx%d, y_off=%d, buf_px=%d",
                 lvgl_w, lvgl_h, lcd_width, lvgl_h * lcd_width / lvgl_w,
                 s_dst_y_off, s_scale_buf_px);

        // Black-fill the full LCD before LVGL starts so the border rows (not touched
        // by scaled flushes) are clean black rather than random GRAM garbage.
        const int chunk_rows  = 16; // 16 rows × 480px × 2B = 15 360 B per DMA write
        const size_t chunk_sz = (size_t)(chunk_rows * lcd_width) * sizeof(lv_color_t);
        lv_color_t *fill_buf  = (lv_color_t*) heap_caps_malloc(chunk_sz, MALLOC_CAP_DMA);
        if (fill_buf) {
            memset(fill_buf, 0, chunk_sz);
            for (int y = 0; y < lcd_height; y += chunk_rows) {
                int end_y = (y + chunk_rows <= lcd_height) ? y + chunk_rows : lcd_height;
                esp_lcd_panel_draw_bitmap(panel_handle, 0, y, lcd_width, end_y, fill_buf);
                vTaskDelay(pdMS_TO_TICKS(5)); // let DMA finish before buffer reuse
            }
            vTaskDelay(pdMS_TO_TICKS(10)); // final settle
            free(fill_buf);
            ESP_LOGI(TAG, "LCD %dx%d black-filled", lcd_width, lcd_height);
        }
    }

    // Configuration is completed.

    ESP_LOGI(TAG, "Install LVGL tick timer");
    // Tick interface for LVGL (using esp_timer to generate 2ms periodic event)
    /*const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &example_increaseLvglTick,
        .name = "lvgl_tick"
    };*/
    esp_timer_handle_t lvgl_tick_timer = NULL;
    // ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    // ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, TDISPLAYS3_LVGL_TICK_PERIOD_MS * 1000));

    ESP_LOGI(TAG, "Display LVGL animation");
    lv_obj_t *scr = lv_disp_get_scr_act(disp);

    return scr;
}

void DisplayDriver::updateHashrate(System *module, StratumManager* manager, float power, int pool)
{
    char strData[20];
    char strDataActive[20];

    float hr = SYSTEM_MODULE.getCurrentHashrate() / 1000.0f;
    float efficiency = (hr > 0) ? power / hr : 10000.0f;
    float hashrate = SYSTEM_MODULE.getCurrentHashrate();
    formatHashrate(strData, sizeof(strData), hashrate);

    lv_label_set_text(m_ui->ui_lbHashrate, strData);    // Update hashrate

    // let it toggle on the pool view page
    if (manager->isDualPool()) {
        auto *m = static_cast<StratumManagerDualPool*>(manager);
        float activeHashrate = m->getActivePoolHashrate(pool);
        formatHashrate(strDataActive, sizeof(strDataActive), activeHashrate);
        lv_label_set_text(m_ui->ui_lbHashrateSet, strDataActive); // Update hashrate
    }

    if (manager->isFallback()) {
        lv_label_set_text(m_ui->ui_lbHashrateSet, strData); // Update hashrate
    }

    lv_label_set_text(m_ui->ui_lblHashPrice, strData);  // Update hashrate

    snprintf(strData, sizeof(strData), "%.1f", efficiency);
    lv_label_set_text(m_ui->ui_lbEficiency, (efficiency < 10000.0f) ? strData : "n/a"); // Update eficiency label

    snprintf(strData, sizeof(strData), "%.3fW", power);
    lv_label_set_text(m_ui->ui_lbPower, strData); // Actualiza el label
}

void DisplayDriver::updateShares(StratumManager *manager, int pool)
{
    char strData[20];

    if (manager->isDualPool()) {
        auto *manager = static_cast<StratumManagerDualPool*>(STRATUM_MANAGER);
        snprintf(strData, sizeof(strData), "%lld/%lld", manager->getSharesAccepted(pool), manager->getSharesRejected(pool));
        lv_label_set_text(m_ui->ui_lbShares, strData); // Update shares
    }

    if (manager->isFallback()) {
        auto *manager = static_cast<StratumManagerFallback*>(STRATUM_MANAGER);
        snprintf(strData, sizeof(strData), "%lld/%lld", manager->getSharesAccepted(), manager->getSharesRejected());
        lv_label_set_text(m_ui->ui_lbShares, strData); // Update shares
    }

    lv_label_set_text(m_ui->ui_lbBestDifficulty, manager->getBestDiffString());    // Update Bestdifficulty
    lv_label_set_text(m_ui->ui_lbBestDifficultySet, manager->getBestDiffString()); // Update Bestdifficulty
}
void DisplayDriver::updateTime(System *module)
{
    char strData[20];

    // Calculate the uptime in seconds
    // int64_t currentTimeTest = esp_timer_get_time() + (8 * 3600 * 1000000LL) + (1800 * 1000000LL);//(8 * 60 * 60 * 10000);
    double uptime_in_seconds = (esp_timer_get_time() - module->getStartTime()) / 1000000;
    int uptime_in_days = uptime_in_seconds / (3600 * 24);
    int remaining_seconds = (int) uptime_in_seconds % (3600 * 24);
    int uptime_in_hours = remaining_seconds / 3600;
    remaining_seconds %= 3600;
    int uptime_in_minutes = remaining_seconds / 60;
    int current_seconds = remaining_seconds % 60;

    snprintf(strData, sizeof(strData), "%dd %ih %im %is", uptime_in_days, uptime_in_hours, uptime_in_minutes, current_seconds);
    lv_label_set_text(m_ui->ui_lbTime, strData); // Update label
}

void DisplayDriver::updateCurrentSettings(int pool)
{
    PThreadGuard lock(m_lvglMutex);
    char strData[20];
    if (m_ui->ui_SettingsScreen == NULL || !STRATUM_MANAGER)
        return;

    Board *board = SYSTEM_MODULE.getBoard();

    if (STRATUM_MANAGER->isDualPool()) {
        auto *manager = static_cast<StratumManagerDualPool*>(STRATUM_MANAGER);
        snprintf(strData, sizeof(strData), "%s", manager->getPoolHost(pool));
        lv_label_set_text(m_ui->ui_lbPoolSet, strData); // Update label
        snprintf(strData, sizeof(strData), "%d", manager->getPoolPort(pool));
        lv_label_set_text(m_ui->ui_lbPortSet, strData); // Update label
        snprintf(strData, sizeof(strData), "%d", pool + 1);
        lv_label_set_text(m_ui->ui_lbPoolNr, strData);
    }

    if (STRATUM_MANAGER->isFallback()) {
        auto *manager = static_cast<StratumManagerFallback*>(STRATUM_MANAGER);
        lv_label_set_text(m_ui->ui_lbPoolSet, manager->getCurrentPoolHost()); // Update label
        snprintf(strData, sizeof(strData), "%d", manager->getCurrentPoolPort());
        lv_label_set_text(m_ui->ui_lbPortSet, strData); // Update label
    }

    snprintf(strData, sizeof(strData), "%d", board->getAsicFrequency());
    lv_label_set_text(m_ui->ui_lbFreqSet, strData); // Update label

    snprintf(strData, sizeof(strData), "%d", board->getAsicVoltageMillis());
    lv_label_set_text(m_ui->ui_lbVcoreSet, strData); // Update label

    switch (m_tempControlMode) {
        case 1:
            lv_label_set_text(m_ui->ui_lbFanSet, "AUTO"); // Update label
            break;
        case 2:
            lv_label_set_text(m_ui->ui_lbFanSet, "PID"); // Update label
            break;
        default:
            snprintf(strData, sizeof(strData), "%d", m_fanSpeed);
            lv_label_set_text(m_ui->ui_lbFanSet, strData); // Update label
            break;
    }
}


void DisplayDriver::updateBTCprice(void)
{
    char price_str[32];

    if ((m_state != UiState::BTCScreen) && (m_btcPrice != 0))
        return;

    m_btcPrice = APIs_FETCHER.getPrice();
    snprintf(price_str, sizeof(price_str), "%u$", m_btcPrice);
    lv_label_set_text(m_ui->ui_lblBTCPrice, price_str); // Update label
}

void DisplayDriver::updateGlobalMiningStats(void)
{
    char strData[32];

    if ((m_state != UiState::GlobalStats) && (m_blockHeight != 0))
        return;


    m_blockHeight = APIs_FETCHER.getBlockHeight();
    snprintf(strData, sizeof(strData), "%lu", m_blockHeight);
    lv_label_set_text(m_ui->ui_lblBlock, strData); // Update label

    snprintf(strData, sizeof(strData), "%lu", APIs_FETCHER.getBlocksToHalving());
    lv_label_set_text(m_ui->ui_lblBlocksToHalving, strData); // Update label

    snprintf(strData, sizeof(strData), "%lu%%", APIs_FETCHER.getHalvingPercent());
    lv_label_set_text(m_ui->ui_lblHalvingPercent, strData); // Update label

    snprintf(strData, sizeof(strData), "%llu", APIs_FETCHER.getNetHash());
    lv_label_set_text(m_ui->ui_lblGlobalHash, strData); // Update label

    snprintf(strData, sizeof(strData), "%lluT", APIs_FETCHER.getNetDifficulty());
    lv_label_set_text(m_ui->ui_lblDifficulty, strData); // Update label

    snprintf(strData, sizeof(strData), "%lu", APIs_FETCHER.getLowestFee());
    lv_label_set_text(m_ui->ui_lbllowFee, strData); // Update label

    snprintf(strData, sizeof(strData), "%lu", APIs_FETCHER.getMidFee());
    lv_label_set_text(m_ui->ui_lblmedFee, strData); // Update label

    snprintf(strData, sizeof(strData), "%lu", APIs_FETCHER.getFastestFee());
    lv_label_set_text(m_ui->ui_lblhighFee, strData); // Update label
}

void DisplayDriver::updateGlobalState(int pool)
{
    PThreadGuard lock(m_lvglMutex);
    char strData[20];

    if (m_ui->ui_MiningScreen == NULL)
        return;
    if (m_ui->ui_SettingsScreen == NULL)
        return;

    // snprintf(strData, sizeof(strData), "%.0f", power_management->chip_temp);
    snprintf(strData, sizeof(strData), "%.0f", POWER_MANAGEMENT_MODULE.getChipTempMax());
    lv_label_set_text(m_ui->ui_lbTemp, strData);       // Update label
    lv_label_set_text(m_ui->ui_lblTempPrice, strData); // Update label

    snprintf(strData, sizeof(strData), "%d", POWER_MANAGEMENT_MODULE.getFanRPM(0));
    lv_label_set_text(m_ui->ui_lbRPM, strData); // Update label

    snprintf(strData, sizeof(strData), "%.3fW", POWER_MANAGEMENT_MODULE.getPower());
    lv_label_set_text(m_ui->ui_lbPower, strData); // Update label

    snprintf(strData, sizeof(strData), "%imA", (int) POWER_MANAGEMENT_MODULE.getCurrent());
    lv_label_set_text(m_ui->ui_lbIntensidad, strData); // Update label

    snprintf(strData, sizeof(strData), "%imV", (int) POWER_MANAGEMENT_MODULE.getVoltage());
    lv_label_set_text(m_ui->ui_lbVinput, strData); // Update label

    updateTime(&SYSTEM_MODULE);
    updateShares(STRATUM_MANAGER, pool);
    updateHashrate(&SYSTEM_MODULE, STRATUM_MANAGER, POWER_MANAGEMENT_MODULE.getPower(), pool);
    updateBTCprice();
    updateGlobalMiningStats();

    Board *board = SYSTEM_MODULE.getBoard();
    uint16_t vcore = (int) (board->getVout() * 1000.0f);
    snprintf(strData, sizeof(strData), "%umV", vcore);
    lv_label_set_text(m_ui->ui_lbVcore, strData); // Update label
}

void DisplayDriver::updateIpAddress(const char *ip_address_str)
{
    PThreadGuard lock(m_lvglMutex);
    if (m_ui->ui_MiningScreen == NULL)
        return;
    if (m_ui->ui_SettingsScreen == NULL)
        return;

    lv_label_set_text(m_ui->ui_lbIP, ip_address_str);    // Update label
    lv_label_set_text(m_ui->ui_lbIPSet, ip_address_str); // Update label
}

void DisplayDriver::logMessage(const char *message)
{
    PThreadGuard lock(m_lvglMutex);
    if (m_ui->ui_LogScreen == NULL)
        m_ui->logScreenInit();
    lv_label_set_text(m_ui->ui_LogLabel, message);
    enableLvglAnimations(true);
    _ui_screen_change(m_ui->ui_LogScreen, LV_SCR_LOAD_ANIM_NONE, 500, 0);
}

void DisplayDriver::miningScreen(void)
{
    PThreadGuard lock(m_lvglMutex);
    enterState(UiState::Mining, now_us());
}


void DisplayDriver::portalScreen(const char *message)
{
    PThreadGuard lock(m_lvglMutex);
    strlcpy(m_portalWifiName, message, sizeof(m_portalWifiName));
    lv_label_set_text(m_ui->ui_lbSSID, m_portalWifiName);
    enterState(UiState::Portal, now_us());
}

void DisplayDriver::updateWifiStatus(const char *message)
{
    PThreadGuard lock(m_lvglMutex);
    if (m_ui->ui_lbConnect != NULL)
        lv_label_set_text(m_ui->ui_lbConnect, message); // Actualiza el label
    refreshScreen();
}

void DisplayDriver::buttonsInit(void)
{
    gpio_pad_select_gpio(PIN_BUTTON_1);
    gpio_set_direction(PIN_BUTTON_1, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_BUTTON_1, GPIO_PULLUP_ONLY);

    gpio_pad_select_gpio(PIN_BUTTON_2);
    gpio_set_direction(PIN_BUTTON_2, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_BUTTON_2, GPIO_PULLUP_ONLY);
}

/**
 * @brief Program starts from here
 *
 */
void DisplayDriver::init(Board* board)
{
    ESP_LOGI("INFO", "Setting Up TDisplayS3 Screen");

    // Inicializa el GPIO para el botón
    buttonsInit();

    // init the ipc
    ui_ipc_init();

    lv_obj_t *scr = initTDisplayS3();

    m_ui = new UI();
    m_ui->init(board, this);
    // manual_lvgl_update();

    // startUpdateScreenTask(); //Start screen update task
    mainCreatSysteTasks();
}
/**************************  Useful Electronics  ****************END OF FILE***/
