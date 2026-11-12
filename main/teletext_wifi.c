#include "teletext_wifi.h"

#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "teletext_wifi";

#define BIT_CONNECTED BIT0

/*
 * esp_hosted internal (host/mcu/eh_host_mcu_transport/src/eh_host_mcu_transport_priv.h):
 * sets the size of the host's SDIO receive buffers. Only a larger value than the current one
 * has an effect after start-up. Used below for older C6 firmware.
 */
void eh_host_bus_sdio_set_rx_staging_target(uint32_t target_size);

/** 20 frames of 1536 bytes: the default C6 TX queue, as esp_hosted sizes streaming buffers. */
#define OLD_CP_RX_STAGING_BYTES (20u * 1536u)

/** Delay before reconnecting after a disconnect, so a wrong password does not spin. */
#define RECONNECT_DELAY_US (5 * 1000 * 1000)

static EventGroupHandle_t s_events;
static esp_timer_handle_t s_reconnect_timer;
/** Connection attempts that failed since the last time the station got an IP address. */
static volatile uint32_t s_failed_attempts;

static void reconnect_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        xEventGroupClearBits(s_events, BIT_CONNECTED);
        s_failed_attempts++;
        ESP_LOGW(TAG, "Not connected (reason %d, failed attempt %lu), retrying in %d s", d ? d->reason : -1,
                 (unsigned long)s_failed_attempts, RECONNECT_DELAY_US / 1000000);
        esp_timer_stop(s_reconnect_timer);
        esp_timer_start_once(s_reconnect_timer, RECONNECT_DELAY_US);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "Connected, IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_failed_attempts = 0;
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

/** Power the C6 and start the esp_hosted SDIO link (auto-init is off in sdkconfig.defaults). */
static esp_err_t start_coprocessor(void)
{
    esp_err_t err = bsp_feature_enable(BSP_FEATURE_WIFI, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "C6 power enable failed: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    if (esp_hosted_init() != 0) {
        ESP_LOGE(TAG, "esp_hosted_init failed");
        return ESP_FAIL;
    }
    if (esp_hosted_connect_to_slave() != 0) {
        ESP_LOGE(TAG, "No answer from the ESP32-C6 (esp_hosted firmware missing?)");
        return ESP_FAIL;
    }

    esp_hosted_coprocessor_fwver_t ver = {0};
    if (esp_hosted_get_coprocessor_fwversion(&ver) == 0) {
        ESP_LOGI(TAG, "ESP32-C6 esp_hosted firmware %lu.%lu.%lu", (unsigned long)ver.major1,
                 (unsigned long)ver.minor1, (unsigned long)ver.patch1);
    }
    if (ver.major1 < 3) {
        eh_host_bus_sdio_set_rx_staging_target(OLD_CP_RX_STAGING_BYTES);
    }
    return ESP_OK;
}

esp_err_t teletext_wifi_start(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_events != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = start_coprocessor();
    if (err != ESP_OK) {
        return err;
    }

    const esp_timer_create_args_t timer_args = {.callback = reconnect_cb, .name = "wifi_reconnect"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_reconnect_timer), TAG, "timer");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL), TAG, "evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL), TAG, "evt");

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    if (password != NULL) {
        strlcpy((char *)wc.sta.password, password, sizeof(wc.sta.password));
    }
    /* WPA2 or better when a password is set (WPA3 networks are accepted too). */
    wc.sta.threshold.authmode = (password != NULL && password[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    ESP_LOGI(TAG, "Connecting to \"%s\"", ssid);
    return ESP_OK;
}

uint32_t teletext_wifi_failed_attempts(void)
{
    return s_failed_attempts;
}

bool teletext_wifi_wait_connected(uint32_t timeout_ms)
{
    if (s_events == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CONNECTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (bits & BIT_CONNECTED) != 0;
}
