#include "teletext_cache.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_spiffs.h"
#include "esp_timer.h"

static const char *TAG = "teletext_cache";

#define BASE_PATH      "/spiffs"
#define PARTITION      "storage"
#define CACHE_NAME     "teletext.bin"
#define CACHE_TMP_NAME "teletext.new"
#define CACHE_FILE     BASE_PATH "/" CACHE_NAME
#define CACHE_TMP_FILE BASE_PATH "/" CACHE_TMP_NAME

#define MAGIC "TTXSET1"
#define MAX_FILE_BYTES (8 * 1024 * 1024)

/*
 * File layout (little endian):
 *   file_header_t
 *   file_page_t   × page_count     catalog, sorted by page
 *   file_pic_t    × pic_count      sorted by (page, part); offset is into the data area
 *   data area: the WebP files back to back
 * crc32 covers everything after the header.
 */
typedef struct {
    char magic[8];
    uint32_t page_count;
    uint32_t pic_count;
    uint32_t data_len;
    uint32_t crc32;
} file_header_t;

typedef struct {
    uint16_t page_id;
    uint8_t num_parts;
    uint8_t reserved;
} file_page_t;

typedef struct {
    uint16_t page_id;
    uint8_t part;
    uint8_t reserved;
    uint32_t offset;
    uint32_t len;
} file_pic_t;

esp_err_t teletext_cache_mount(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = BASE_PATH,
        .partition_label = PARTITION,
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Older firmware flashed pages.csv and PNG pictures here; only the cache files belong now. */
    DIR *dir = opendir(BASE_PATH);
    if (dir != NULL) {
        unsigned removed = 0;
        struct dirent *e;
        char path[300];
        while ((e = readdir(dir)) != NULL) {
            if (strcmp(e->d_name, CACHE_NAME) == 0 || strcmp(e->d_name, CACHE_TMP_NAME) == 0) {
                continue;
            }
            snprintf(path, sizeof(path), BASE_PATH "/%s", e->d_name);
            if (remove(path) == 0) {
                removed++;
            }
        }
        closedir(dir);
        if (removed > 0) {
            ESP_LOGI(TAG, "Removed %u old file(s) from SPIFFS", removed);
        }
    }

    size_t total = 0, used = 0;
    if (esp_spiffs_info(PARTITION, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS: %u / %u bytes used", (unsigned)used, (unsigned)total);
    }
    return ESP_OK;
}

static uint8_t *read_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = NULL;
    if (len > (long)sizeof(file_header_t) && len <= MAX_FILE_BYTES) {
        buf = heap_caps_malloc((size_t)len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf != NULL && fread(buf, 1, (size_t)len, f) != (size_t)len) {
            heap_caps_free(buf);
            buf = NULL;
        }
    }
    fclose(f);
    *len_out = (size_t)len;
    return buf;
}

/** Validate a cache file in memory and build catalog + set from it. Takes ownership of @a buf. */
static esp_err_t parse_file(uint8_t *buf, size_t len, teletext_catalog_t **cat_out, teletext_net_set_t **set_out)
{
    const file_header_t *h = (const file_header_t *)buf;
    const size_t pages_off = sizeof(*h);
    const size_t pics_off = pages_off + (size_t)h->page_count * sizeof(file_page_t);
    const size_t data_off = pics_off + (size_t)h->pic_count * sizeof(file_pic_t);

    if (memcmp(h->magic, MAGIC, sizeof(MAGIC)) != 0 || h->page_count == 0 || h->page_count > 10000 ||
        h->pic_count > 65535 || data_off > len || len - data_off != h->data_len ||
        esp_rom_crc32_le(0, buf + pages_off, len - pages_off) != h->crc32) {
        heap_caps_free(buf);
        return ESP_ERR_INVALID_CRC;
    }

    teletext_catalog_t *cat = heap_caps_calloc(1, sizeof(*cat), MALLOC_CAP_SPIRAM);
    teletext_net_set_t *set = heap_caps_calloc(1, sizeof(*set), MALLOC_CAP_SPIRAM);
    if (cat != NULL) {
        cat->entries = heap_caps_calloc(h->page_count, sizeof(*cat->entries), MALLOC_CAP_SPIRAM);
    }
    if (set != NULL) {
        set->pics = heap_caps_calloc(h->pic_count ? h->pic_count : 1, sizeof(*set->pics), MALLOC_CAP_SPIRAM);
    }
    if (cat == NULL || cat->entries == NULL || set == NULL || set->pics == NULL) {
        if (cat != NULL) {
            heap_caps_free(cat->entries);
        }
        heap_caps_free(cat);
        teletext_net_set_free(set);
        heap_caps_free(buf);
        return ESP_ERR_NO_MEM;
    }

    const file_page_t *pages = (const file_page_t *)(buf + pages_off);
    for (uint32_t i = 0; i < h->page_count; i++) {
        cat->entries[i].page_id = pages[i].page_id;
        cat->entries[i].num_parts = pages[i].num_parts ? pages[i].num_parts : 1;
    }
    cat->count = h->page_count;

    /* The pictures stay in the file buffer; the set owns it as its blob. */
    const file_pic_t *pics = (const file_pic_t *)(buf + pics_off);
    uint8_t *data = buf + data_off;
    for (uint32_t i = 0; i < h->pic_count; i++) {
        if (pics[i].offset > h->data_len || pics[i].len > h->data_len - pics[i].offset) {
            continue;
        }
        set->pics[set->count++] = (teletext_net_pic_t){
            .page_id = pics[i].page_id, .part = pics[i].part, .len = pics[i].len, .data = data + pics[i].offset};
    }
    set->blob = buf;

    *cat_out = cat;
    *set_out = set;
    return ESP_OK;
}

esp_err_t teletext_cache_load(teletext_catalog_t **cat, teletext_net_set_t **set)
{
    /* teletext.new is only left behind if power failed between removing the old file and the
     * rename; it is complete then, and the CRC decides either way. */
    static const char *const files[] = {CACHE_FILE, CACHE_TMP_FILE};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        const int64_t t0 = esp_timer_get_time();
        size_t len = 0;
        uint8_t *buf = read_file(files[i], &len);
        if (buf == NULL) {
            continue;
        }
        esp_err_t err = parse_file(buf, len, cat, set);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Loaded %s: %u pages, %u pictures, %u KB in %lld ms", files[i], (unsigned)(*cat)->count,
                     (unsigned)(*set)->count, (unsigned)(len / 1024), (esp_timer_get_time() - t0) / 1000);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "%s is not valid (%s)", files[i], esp_err_to_name(err));
    }
    return ESP_ERR_NOT_FOUND;
}

static bool write_all(FILE *f, const void *data, size_t len)
{
    return len == 0 || fwrite(data, 1, len, f) == len;
}

esp_err_t teletext_cache_save(const teletext_catalog_t *cat, const teletext_net_set_t *set)
{
    if (cat == NULL || set == NULL || cat->count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const int64_t t0 = esp_timer_get_time();

    const size_t pages_bytes = cat->count * sizeof(file_page_t);
    const size_t pics_bytes = set->count * sizeof(file_pic_t);
    file_page_t *pages = heap_caps_calloc(1, pages_bytes, MALLOC_CAP_SPIRAM);
    file_pic_t *pics = heap_caps_calloc(1, pics_bytes ? pics_bytes : 1, MALLOC_CAP_SPIRAM);
    if (pages == NULL || pics == NULL) {
        heap_caps_free(pages);
        heap_caps_free(pics);
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < cat->count; i++) {
        pages[i] = (file_page_t){.page_id = cat->entries[i].page_id, .num_parts = cat->entries[i].num_parts};
    }
    uint32_t offset = 0;
    for (size_t i = 0; i < set->count; i++) {
        const teletext_net_pic_t *p = &set->pics[i];
        pics[i] = (file_pic_t){.page_id = p->page_id, .part = p->part, .offset = offset, .len = p->len};
        offset += p->len;
    }

    file_header_t h = {.page_count = (uint32_t)cat->count, .pic_count = (uint32_t)set->count, .data_len = offset};
    memcpy(h.magic, MAGIC, sizeof(MAGIC));
    uint32_t crc = esp_rom_crc32_le(0, (const uint8_t *)pages, pages_bytes);
    crc = esp_rom_crc32_le(crc, (const uint8_t *)pics, pics_bytes);
    for (size_t i = 0; i < set->count; i++) {
        crc = esp_rom_crc32_le(crc, set->pics[i].data, set->pics[i].len);
    }
    h.crc32 = crc;

    /* Make room first: SPIFFS needs free space for the new file next to the current one. */
    remove(CACHE_TMP_FILE);
    esp_err_t err = ESP_FAIL;
    FILE *f = fopen(CACHE_TMP_FILE, "wb");
    if (f != NULL) {
        bool ok = write_all(f, &h, sizeof(h)) && write_all(f, pages, pages_bytes) && write_all(f, pics, pics_bytes);
        for (size_t i = 0; ok && i < set->count; i++) {
            ok = write_all(f, set->pics[i].data, set->pics[i].len);
        }
        ok = (fclose(f) == 0) && ok;
        if (ok) {
            remove(CACHE_FILE);
            err = (rename(CACHE_TMP_FILE, CACHE_FILE) == 0) ? ESP_OK : ESP_FAIL;
        } else {
            remove(CACHE_TMP_FILE);
        }
    }
    heap_caps_free(pages);
    heap_caps_free(pics);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved %u pages, %u pictures (%u KB) in %lld ms", (unsigned)cat->count, (unsigned)set->count,
                 (unsigned)((sizeof(h) + pages_bytes + pics_bytes + offset) / 1024),
                 (esp_timer_get_time() - t0) / 1000);
    } else {
        ESP_LOGE(TAG, "Saving the page set failed (SPIFFS full?)");
    }
    return err;
}
