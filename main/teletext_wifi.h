#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * Wi-Fi station through the Tab5's ESP32-C6 (esp_hosted over SDIO + esp_wifi_remote).
 *
 * Powers the C6 through the IO expander, starts esp_hosted, then connects and keeps
 * reconnecting in the background. Blocks for a few seconds while the C6 starts, so
 * call it from a task, not from the LVGL task.
 */
esp_err_t teletext_wifi_start(const char *ssid, const char *password);

/**
 * Connection attempts that failed in a row (router off, wrong password, out of range, or a
 * dropped connection). Reset to 0 when the station gets an IP address. Retries every 5 s.
 */
uint32_t teletext_wifi_failed_attempts(void);

/** Wait until the station has an IP address. @return true when connected. */
bool teletext_wifi_wait_connected(uint32_t timeout_ms);
