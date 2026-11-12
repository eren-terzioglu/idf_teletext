/*
 * Teletext viewer for the M5Stack Tab5 — LVGL 9, pages downloaded on the device over Wi-Fi.
 * Landscape 1280×720: teletext picture on the left, keypad on the right.
 *
 * Boot: the UI starts empty; the teletext_online task shows the last saved page set from
 * SPIFFS (if any), downloads a fresh set, shows it and saves it for the next boot.
 */

#include "esp_log.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "tab5_display.h"
#include "teletext_online.h"
#include "teletext_ui.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_err = nvs_flash_init();
    }
    if (nvs_err != ESP_OK) {
        ESP_LOGW(TAG, "NVS init failed: %s", esp_err_to_name(nvs_err));
    }

    lv_display_t *disp = NULL;
    esp_err_t disp_err = tab5_display_start(&disp);
    if (disp_err != ESP_OK || disp == NULL) {
        ESP_LOGE(TAG, "Display start failed: %s", esp_err_to_name(disp_err));
        return;
    }
    ESP_LOGI(TAG, "Panel: %s", tab5_display_get_panel_name());

    /* The panel is 720×1280 portrait; rotate to 1280×720 landscape (touch follows). */
    bsp_display_rotate(disp, LV_DISPLAY_ROTATION_90);

    bsp_display_lock(0);
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    teletext_ui_init(scr, NULL);
    bsp_display_unlock();

    (void) bsp_display_backlight_on();
    ESP_LOGI(TAG, "Teletext UI ready");

    /* Saved pages first, then Wi-Fi + download in the background. */
    teletext_online_start();
}
