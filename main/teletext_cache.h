#pragma once

#include "esp_err.h"
#include "teletext_pages.h"
#include "teletext_store.h"

/**
 * Last downloaded page set, kept in the SPIFFS `storage` partition as one file
 * (/spiffs/teletext.bin: catalog + all WebP pictures, CRC-checked), so the viewer has pages
 * right after boot and when Wi-Fi is not available.
 */

/** Mount SPIFFS (formats an empty partition) and delete files this firmware does not use. */
esp_err_t teletext_cache_mount(void);

/**
 * Load the saved set. On success the caller owns *cat (entries + struct, free with
 * heap_caps_free) and *set (teletext_net_set_free).
 * @return ESP_ERR_NOT_FOUND when nothing valid is saved.
 */
esp_err_t teletext_cache_load(teletext_catalog_t **cat, teletext_net_set_t **set);

/** Save a set, replacing the previous one only after the new file is completely written. */
esp_err_t teletext_cache_save(const teletext_catalog_t *cat, const teletext_net_set_t *set);
