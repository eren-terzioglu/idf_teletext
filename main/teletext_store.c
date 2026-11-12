#include "teletext_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "webp/decode.h"

static const char *TAG = "teletext_store";

/** Installed pictures; only replaced with the display lock held (see teletext_store.h). */
static teletext_net_set_t *s_net_set;

static inline uint16_t rgb888_to_565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/**
 * Nearest-neighbour fit of a src_w×src_h RGBA picture into dst (aspect kept, centred, black border).
 * @a stride is bytes per source row.
 */
static void scale_into(const uint8_t *src, uint32_t src_w, uint32_t src_h, uint32_t stride, uint16_t *dst,
                       uint16_t dst_w, uint16_t dst_h)
{
    /* Largest size that fits while keeping the aspect ratio. */
    uint32_t out_w = dst_w;
    uint32_t out_h = (uint32_t)((uint64_t)src_h * dst_w / src_w);
    if (out_h > dst_h) {
        out_h = dst_h;
        out_w = (uint32_t)((uint64_t)src_w * dst_h / src_h);
    }
    const uint32_t off_x = (dst_w - out_w) / 2;
    const uint32_t off_y = (dst_h - out_h) / 2;

    /* Only the borders need clearing; the picture area is fully overwritten. */
    if (off_y > 0) {
        memset(dst, 0, (size_t)dst_w * off_y * sizeof(uint16_t));
        memset(dst + (size_t)(off_y + out_h) * dst_w, 0, (size_t)dst_w * (dst_h - off_y - out_h) * sizeof(uint16_t));
    }

    /* Source byte offset of every output column, computed once. */
    uint16_t *col = malloc(out_w * sizeof(uint16_t));
    if (col == NULL) {
        return;
    }
    for (uint32_t x = 0; x < out_w; x++) {
        col[x] = (uint16_t)((x * src_w / out_w) * 4);
    }

    /* Converted output row, built in internal RAM, then copied to every row that maps to it. */
    uint16_t *line = heap_caps_malloc(out_w * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (line == NULL) {
        free(col);
        return;
    }

    uint32_t prev_sy = UINT32_MAX;
    for (uint32_t y = 0; y < out_h; y++) {
        const uint32_t sy = y * src_h / out_h;
        uint16_t *drow = dst + (size_t)(y + off_y) * dst_w;
        if (sy != prev_sy) {
            const uint8_t *srow = src + (size_t)sy * stride;
            for (uint32_t x = 0; x < out_w; x++) {
                const uint8_t *p = srow + col[x];
                line[x] = rgb888_to_565(p[0], p[1], p[2]);
            }
            prev_sy = sy;
        }
        if (off_x > 0) {
            memset(drow, 0, off_x * sizeof(uint16_t));
            memset(drow + off_x + out_w, 0, (dst_w - off_x - out_w) * sizeof(uint16_t));
        }
        memcpy(drow + off_x, line, out_w * sizeof(uint16_t));
    }

    heap_caps_free(line);
    free(col);
}

void teletext_net_set_free(teletext_net_set_t *set)
{
    if (set == NULL) {
        return;
    }
    if (set->blob != NULL) {
        heap_caps_free(set->blob);
    } else if (set->pics != NULL) {
        for (size_t i = 0; i < set->count; i++) {
            heap_caps_free(set->pics[i].data);
        }
    }
    heap_caps_free(set->pics);
    heap_caps_free(set);
}

void teletext_store_install_net_set(teletext_net_set_t *set)
{
    teletext_net_set_t *old = s_net_set;
    s_net_set = set;
    teletext_net_set_free(old);
}

const teletext_net_pic_t *teletext_store_find_pic(uint16_t page_id, uint8_t part)
{
    if (s_net_set == NULL) {
        return NULL;
    }
    const uint32_t key = ((uint32_t)page_id << 8) | part;
    size_t lo = 0, hi = s_net_set->count;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        const teletext_net_pic_t *p = &s_net_set->pics[mid];
        const uint32_t k = ((uint32_t)p->page_id << 8) | p->part;
        if (k == key) {
            return p;
        }
        if (k < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

esp_err_t teletext_load_page_image(uint16_t page_id, uint8_t part_1based, uint8_t num_parts,
                                   uint16_t *dst, uint16_t dst_w, uint16_t dst_h)
{
    if (dst == NULL || dst_w == 0 || dst_h == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t part = (num_parts <= 1 || part_1based < 1) ? 1 : part_1based;
    const teletext_net_pic_t *pic = teletext_store_find_pic(page_id, part);
    if (pic == NULL) {
        ESP_LOGW(TAG, "No picture for page %u part %u", page_id, (unsigned)part);
        return ESP_ERR_NOT_FOUND;
    }

    const int64_t t0 = esp_timer_get_time();
    int w = 0, h = 0;
    /* The ~340 KB RGBA output goes to PSRAM through malloc (CONFIG_SPIRAM_USE_MALLOC). */
    uint8_t *rgba = WebPDecodeRGBA(pic->data, pic->len, &w, &h);
    if (rgba == NULL || w <= 0 || h <= 0 || w > 2048 || h > 2048) {
        ESP_LOGE(TAG, "Page %u-%u: WebP decode failed", page_id, (unsigned)part);
        WebPFree(rgba);
        return ESP_FAIL;
    }
    const int64_t t1 = esp_timer_get_time();
    scale_into(rgba, (uint32_t)w, (uint32_t)h, (uint32_t)w * 4, dst, dst_w, dst_h);
    WebPFree(rgba);
    ESP_LOGI(TAG, "Page %u-%u %dx%d: decode %lld ms, scale %lld ms", page_id, (unsigned)part, w, h,
             (t1 - t0) / 1000, (esp_timer_get_time() - t1) / 1000);
    return ESP_OK;
}
