/*
 * On-device download of the ČT teletext pages.
 *
 *   index:   <CONFIG_TELETEXT_API_URL>                      → {"data": {"<page>": {"subpages": ["A", ...]}}}
 *   picture: <CONFIG_TELETEXT_API_URL>/<page><suffix>/image.webp
 *
 * Boot: show the set saved in SPIFFS (teletext_cache) right away, then download a fresh set
 * (~1.7 MB of WebP) into PSRAM. Once enough pictures arrived it is swapped into the UI in one
 * step and saved for the next boot. If Wi-Fi or the download fails, the saved set stays.
 */
#include "teletext_online.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "bsp/esp-bsp.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "teletext_cache.h"
#include "teletext_pages.h"
#include "teletext_store.h"
#include "teletext_ui.h"
#include "teletext_wifi.h"

static const char *TAG = "teletext_online";

#define INDEX_MAX_BYTES   (1024 * 1024)
#define PICTURE_MAX_BYTES (256 * 1024)
#define HTTP_TRIES        4
/** Pause between pictures: be gentle with the server. */
#define PICTURE_GAP_MS    50
/** How often the task checks the Wi-Fi state while not connected. */
#define WIFI_POLL_MS      1000
/** Failed connection attempts (one every ~5 s) before the error is shown, once. */
#define WIFI_FAILS_BEFORE_ERROR 15
/** Every status note and error disappears after this time. */
#define NOTICE_SHOW_MS          10000

/** One picture to download: <page><suffix> → part. */
typedef struct {
    uint16_t page_id;
    uint8_t part;
    char suffix[4];
} pic_task_t;

/* ---------- PSRAM helpers ---------- */

static void *psram_malloc(size_t n)
{
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void *psram_calloc(size_t n, size_t size)
{
    return heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/* ---------- HTTP ---------- */

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    size_t max;
    bool overflow;
} body_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    body_t *b = evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || b == NULL || b->overflow) {
        return ESP_OK;
    }
    if (b->len + evt->data_len > b->max) {
        b->overflow = true;
        return ESP_OK;
    }
    if (b->len + evt->data_len > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 8192;
        while (ncap < b->len + evt->data_len) {
            ncap *= 2;
        }
        uint8_t *nd = heap_caps_realloc(b->data, ncap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (nd == NULL) {
            b->overflow = true;
            return ESP_OK;
        }
        b->data = nd;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, evt->data, evt->data_len);
    b->len += evt->data_len;
    return ESP_OK;
}

/**
 * GET @a url into a PSRAM buffer (caller frees with heap_caps_free). The client is reused, so
 * all requests to the API share one keep-alive TLS connection while the server allows it.
 */
static esp_err_t http_get(esp_http_client_handle_t client, const char *url, size_t max, uint8_t **out,
                          size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    uint32_t delay_ms = 1000;
    esp_err_t err = ESP_FAIL;

    for (int attempt = 1; attempt <= HTTP_TRIES; attempt++) {
        body_t body = {.max = max};
        esp_http_client_set_user_data(client, &body);
        esp_http_client_set_url(client, url);
        err = esp_http_client_perform(client);
        const int status = esp_http_client_get_status_code(client);

        if (err == ESP_OK && status == 200 && !body.overflow && body.len > 0) {
            *out = body.data;
            *out_len = body.len;
            return ESP_OK;
        }
        heap_caps_free(body.data);
        if (err == ESP_OK) {
            err = (status == 200) ? ESP_ERR_INVALID_SIZE : ESP_ERR_INVALID_RESPONSE;
        }
        /* 4xx will not change on retry. */
        if (status >= 400 && status < 500) {
            ESP_LOGW(TAG, "GET %s: HTTP %d", url, status);
            return err;
        }
        ESP_LOGW(TAG, "GET %s: %s (HTTP %d), try %d/%d", url, esp_err_to_name(err), status, attempt, HTTP_TRIES);
        /* Drop a connection the server may have closed; the next perform reconnects. */
        esp_http_client_close(client);
        if (attempt < HTTP_TRIES) {
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            delay_ms *= 2;
        }
    }
    return err;
}

/* ---------- index ---------- */

static bool parse_page_key(const char *key, uint16_t *out)
{
    size_t n = strlen(key);
    if (n == 0 || n > 4) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!isdigit((unsigned char)key[i])) {
            return false;
        }
    }
    *out = (uint16_t)atoi(key);
    return true;
}

static int cmp_entry(const void *a, const void *b)
{
    return (int)((const teletext_page_entry_t *)a)->page_id - (int)((const teletext_page_entry_t *)b)->page_id;
}

static int cmp_task(const void *a, const void *b)
{
    const pic_task_t *x = a, *y = b;
    if (x->page_id != y->page_id) {
        return (int)x->page_id - (int)y->page_id;
    }
    return (int)x->part - (int)y->part;
}

/**
 * Parse the index JSON into a catalog (sorted by page) and the list of
 * pictures to fetch: no subpages → one picture with no
 * suffix; otherwise one picture per subpage letter, numbered 1.. in index order.
 */
static esp_err_t parse_index(const uint8_t *json, size_t len, teletext_catalog_t *cat, pic_task_t **tasks_out,
                             size_t *ntasks_out)
{
    cJSON *root = cJSON_ParseWithLength((const char *)json, len);
    if (root == NULL) {
        ESP_LOGE(TAG, "Index: invalid JSON");
        return ESP_FAIL;
    }
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsObject(data)) {
        ESP_LOGE(TAG, "Index: no \"data\" object");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    const int npages = cJSON_GetArraySize(data);
    size_t ntasks = 0;
    const cJSON *page;
    cJSON_ArrayForEach(page, data) {
        const cJSON *subs = cJSON_GetObjectItemCaseSensitive(page, "subpages");
        const int ns = cJSON_IsArray(subs) ? cJSON_GetArraySize(subs) : 0;
        ntasks += (ns > 0) ? (size_t)ns : 1;
    }

    teletext_page_entry_t *entries = psram_calloc(npages > 0 ? npages : 1, sizeof(*entries));
    pic_task_t *tasks = psram_calloc(ntasks > 0 ? ntasks : 1, sizeof(*tasks));
    if (entries == NULL || tasks == NULL) {
        heap_caps_free(entries);
        heap_caps_free(tasks);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    size_t ne = 0, nt = 0;
    cJSON_ArrayForEach(page, data) {
        uint16_t page_id;
        if (page->string == NULL || !parse_page_key(page->string, &page_id)) {
            continue;
        }
        const cJSON *subs = cJSON_GetObjectItemCaseSensitive(page, "subpages");
        const int ns = cJSON_IsArray(subs) ? cJSON_GetArraySize(subs) : 0;
        if (ns > 255) {
            ESP_LOGW(TAG, "Page %u: %d subpages, skipped", page_id, ns);
            continue;
        }
        if (ns == 0) {
            tasks[nt++] = (pic_task_t){.page_id = page_id, .part = 1, .suffix = ""};
        } else {
            uint8_t part = 0;
            const cJSON *s;
            cJSON_ArrayForEach(s, subs) {
                pic_task_t *t = &tasks[nt++];
                t->page_id = page_id;
                t->part = ++part;
                if (cJSON_IsString(s) && strlen(s->valuestring) < sizeof(t->suffix)) {
                    strlcpy(t->suffix, s->valuestring, sizeof(t->suffix));
                } else {
                    t->suffix[0] = '\0';
                }
            }
        }
        entries[ne].page_id = page_id;
        entries[ne].num_parts = (uint8_t)(ns > 0 ? ns : 1);
        ne++;
    }
    cJSON_Delete(root);

    /* The API index is unordered; page -/+ follows catalog order. */
    qsort(entries, ne, sizeof(*entries), cmp_entry);
    qsort(tasks, nt, sizeof(*tasks), cmp_task);

    cat->entries = entries;
    cat->count = ne;
    *tasks_out = tasks;
    *ntasks_out = nt;
    return (ne > 0) ? ESP_OK : ESP_FAIL;
}

/* ---------- UI ---------- */

/**
 * Status note at the top of the picture. Every note disappears after NOTICE_SHOW_MS, so the
 * screen is clean when the device is offline; the download counter stays visible while it is
 * updated (every 25 pictures).
 */
static void notice(const char *text)
{
    if (bsp_display_lock(1000)) {
        teletext_ui_set_notice(text, NOTICE_SHOW_MS);
        bsp_display_unlock();
    }
}

/* ---------- showing a set ---------- */

/** Catalog currently shown; this module allocates and frees every catalog. */
static teletext_catalog_t *s_shown_cat;

/** Swap pictures and catalog together, under the display lock the UI runs with. Takes ownership. */
static bool show_set(teletext_catalog_t *cat, teletext_net_set_t *set, const char *note)
{
    if (!bsp_display_lock(5000)) {
        teletext_catalog_destroy(cat);
        teletext_net_set_free(set);
        return false;
    }
    teletext_store_install_net_set(set);
    teletext_catalog_t *old = teletext_ui_set_catalog(cat);
    teletext_ui_set_notice(note, 4000);
    bsp_display_unlock();

    if (old != NULL && old == s_shown_cat) {
        teletext_catalog_destroy(old);
    }
    s_shown_cat = cat;
    return true;
}

/* ---------- one full update ---------- */

static bool is_webp(const uint8_t *d, size_t n)
{
    return n >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WEBP", 4) == 0;
}

/**
 * Copy of the picture in the currently shown set, used when a download failed so a page does
 * not go blank just because one request failed. This task is the only one that installs sets,
 * so the shown set cannot be freed while it is read here.
 */
static uint8_t *copy_shown_pic(uint16_t page_id, uint8_t part, size_t *len)
{
    const teletext_net_pic_t *p = teletext_store_find_pic(page_id, part);
    if (p == NULL) {
        return NULL;
    }
    uint8_t *d = psram_malloc(p->len);
    if (d != NULL) {
        memcpy(d, p->data, p->len);
        *len = p->len;
    }
    return d;
}

static esp_err_t run_update(void)
{
    const char *base = CONFIG_TELETEXT_API_URL;
    const int64_t t0 = esp_timer_get_time();

    esp_http_client_config_t cfg = {
        .url = base,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event,
        .timeout_ms = 20000,
        .keep_alive_enable = true,
        .user_agent = "teletext-tab5/1.0 (+embedded viewer)",
        .buffer_size = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = ESP_FAIL;
    uint8_t *json = NULL;
    size_t json_len = 0;
    teletext_catalog_t *cat = NULL;
    pic_task_t *tasks = NULL;
    size_t ntasks = 0;
    teletext_net_set_t *set = NULL;

    notice(LV_SYMBOL_WIFI "  Loading page index...");
    if (http_get(client, base, INDEX_MAX_BYTES, &json, &json_len) != ESP_OK) {
        notice(LV_SYMBOL_WARNING "  Page index download failed");
        goto out;
    }
    cat = psram_calloc(1, sizeof(*cat));
    if (cat == NULL || parse_index(json, json_len, cat, &tasks, &ntasks) != ESP_OK) {
        notice(LV_SYMBOL_WARNING "  Page index not readable");
        goto out;
    }
    heap_caps_free(json);
    json = NULL;
    ESP_LOGI(TAG, "Index: %u pages, %u pictures", (unsigned)cat->count, (unsigned)ntasks);

    set = psram_calloc(1, sizeof(*set));
    if (set == NULL || (set->pics = psram_calloc(ntasks, sizeof(*set->pics))) == NULL) {
        goto out;
    }

    size_t fresh = 0, kept = 0, failed = 0;
    size_t bytes = 0;
    for (size_t i = 0; i < ntasks; i++) {
        const pic_task_t *t = &tasks[i];
        char url[160];
        snprintf(url, sizeof(url), "%s/%u%s/image.webp", base, t->page_id, t->suffix);

        uint8_t *data = NULL;
        size_t len = 0;
        if (http_get(client, url, PICTURE_MAX_BYTES, &data, &len) == ESP_OK && is_webp(data, len)) {
            fresh++;
        } else {
            heap_caps_free(data);
            data = copy_shown_pic(t->page_id, t->part, &len);
            if (data != NULL) {
                kept++;
            } else {
                failed++;
            }
        }
        if (data != NULL) {
            /* Sorted tasks → sorted set, as teletext_store's binary search needs. */
            set->pics[set->count++] = (teletext_net_pic_t){
                .page_id = t->page_id, .part = t->part, .len = (uint32_t)len, .data = data};
            bytes += len;
        }

        if ((i + 1) % 25 == 0 || i + 1 == ntasks) {
            char msg[64];
            snprintf(msg, sizeof(msg), LV_SYMBOL_DOWNLOAD "  %u / %u", (unsigned)(i + 1), (unsigned)ntasks);
            notice(msg);
        }
        vTaskDelay(pdMS_TO_TICKS(PICTURE_GAP_MS));
    }

    const unsigned pct = ntasks ? (unsigned)(fresh * 100 / ntasks) : 0;
    ESP_LOGI(TAG, "Downloaded %u/%u pictures (%u KB; %u kept from before, %u missing) in %lld s",
             (unsigned)fresh, (unsigned)ntasks, (unsigned)(bytes / 1024), (unsigned)kept, (unsigned)failed,
             (esp_timer_get_time() - t0) / 1000000);
    if (pct < CONFIG_TELETEXT_MIN_SUCCESS_PERCENT) {
        ESP_LOGW(TAG, "Only %u%% downloaded, keeping the current pages", pct);
        notice(LV_SYMBOL_WARNING "  Download incomplete, kept old pages");
        goto out;
    }

    /* The HTTPS connection is not needed while saving. */
    esp_http_client_cleanup(client);
    client = NULL;

    teletext_catalog_t *shown_cat = cat;
    teletext_net_set_t *shown_set = set;
    cat = NULL;
    set = NULL;
    if (!show_set(shown_cat, shown_set, LV_SYMBOL_OK "  Pages updated")) {
        goto out;
    }
    /* Shown set and catalog stay valid: only this task replaces them. */
    teletext_cache_save(shown_cat, shown_set);
    result = ESP_OK;

out:
    if (client != NULL) {
        esp_http_client_cleanup(client);
    }
    heap_caps_free(json);
    heap_caps_free(tasks);
    teletext_net_set_free(set);
    teletext_catalog_destroy(cat);
    return result;
}

/* ---------- task ---------- */

static void online_task(void *arg)
{
    (void)arg;

    /* cJSON makes one small allocation per JSON value (the index has ~10k); keep them out of
     * internal RAM. Nothing else in this firmware uses cJSON. */
    cJSON_Hooks hooks = {.malloc_fn = psram_malloc, .free_fn = heap_caps_free};
    cJSON_InitHooks(&hooks);

    /* 1. Last saved set, so there is something to read while Wi-Fi starts. */
    bool showing_saved = false;
    if (teletext_cache_mount() == ESP_OK) {
        teletext_catalog_t *cat = NULL;
        teletext_net_set_t *set = NULL;
        if (teletext_cache_load(&cat, &set) == ESP_OK) {
            showing_saved = show_set(cat, set, LV_SYMBOL_SAVE "  Saved pages");
        }
    }
    if (!showing_saved) {
        ESP_LOGI(TAG, "No saved pages yet");
    }

    /* 2. Fresh set over Wi-Fi. */
    if (CONFIG_TELETEXT_WIFI_SSID[0] == '\0') {
        ESP_LOGW(TAG, "No Wi-Fi SSID configured (see sdkconfig.defaults); only saved pages are shown");
        notice(LV_SYMBOL_WARNING "  No Wi-Fi configured");
        vTaskDelete(NULL);
        return;
    }
    notice(LV_SYMBOL_WIFI "  Connecting to Wi-Fi...");
    esp_err_t err = teletext_wifi_start(CONFIG_TELETEXT_WIFI_SSID, CONFIG_TELETEXT_WIFI_PASSWORD);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(err));
        notice(showing_saved ? LV_SYMBOL_WARNING "  Wi-Fi not available, showing saved pages"
                             : LV_SYMBOL_WARNING "  Wi-Fi not available, no saved pages");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        /*
         * Not connected: the Wi-Fi module retries every 5 s on its own. After 15 failed attempts
         * in a row the error is shown once (for NOTICE_SHOW_MS); after that nothing is shown while it keeps
         * retrying. A successful connection resets the count, so a later outage shows it again.
         */
        bool error_shown = false;
        while (!teletext_wifi_wait_connected(WIFI_POLL_MS)) {
            if (!error_shown && teletext_wifi_failed_attempts() >= WIFI_FAILS_BEFORE_ERROR) {
                ESP_LOGW(TAG, "Wi-Fi: %d attempts failed, still retrying in the background",
                         WIFI_FAILS_BEFORE_ERROR);
                notice(s_shown_cat != NULL ? LV_SYMBOL_WARNING "  Wi-Fi connection failed, showing saved pages"
                                           : LV_SYMBOL_WARNING "  Wi-Fi connection failed, no saved pages");
                error_shown = true;
            }
        }
        if (run_update() != ESP_OK) {
            /* Try again sooner than the normal refresh. */
            vTaskDelay(pdMS_TO_TICKS(60 * 1000));
            continue;
        }
        if (CONFIG_TELETEXT_REFRESH_MINUTES == 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS((uint32_t)CONFIG_TELETEXT_REFRESH_MINUTES * 60 * 1000));
    }
    vTaskDelete(NULL);
}

void teletext_online_start(void)
{
    /* SPIFFS, TLS handshake and cJSON run here; the stack stays in internal RAM. */
    if (xTaskCreate(online_task, "teletext_online", 8192, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Task create failed");
    }
}
