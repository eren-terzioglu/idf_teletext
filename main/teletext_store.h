#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** Size of the pictures the ČT API serves (pages/<n>/image.webp). */
#define TELETEXT_SRC_W 312
#define TELETEXT_SRC_H 276

/** One picture: the WebP file as served by the API, in PSRAM. */
typedef struct {
    uint16_t page_id;
    uint8_t part; /**< 1-based */
    uint32_t len;
    uint8_t *data;
} teletext_net_pic_t;

/** A complete picture set, sorted by (page_id, part). */
typedef struct {
    teletext_net_pic_t *pics;
    size_t count;
    /** When set, every pics[i].data points into this one buffer (set loaded from the cache). */
    uint8_t *blob;
} teletext_net_set_t;

void teletext_net_set_free(teletext_net_set_t *set);

/**
 * Make @a set the picture set shown (ownership moves to the store; the previous set is freed).
 * Pictures are read from the LVGL task, so the caller must hold the display lock.
 */
void teletext_store_install_net_set(teletext_net_set_t *set);

/**
 * Picture of the installed set, or NULL. Hold the display lock, or be the only task that
 * installs sets (the result stays valid until the next install).
 */
const teletext_net_pic_t *teletext_store_find_pic(uint16_t page_id, uint8_t part_1based);

/**
 * Decode one picture of the installed set and scale it (nearest neighbour, aspect kept,
 * centred, black borders) into an RGB565 buffer of dst_w × dst_h pixels.
 *
 * @return ESP_OK, ESP_ERR_NOT_FOUND when the set has no such picture, or ESP_FAIL if decoding failed.
 */
esp_err_t teletext_load_page_image(uint16_t page_id, uint8_t part_1based, uint8_t num_parts,
                                   uint16_t *dst, uint16_t dst_w, uint16_t dst_h);
