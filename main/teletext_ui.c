#include "teletext_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "teletext_store.h"

static const char *TAG = "teletext_ui";

/** Picture scale ×10: 25 → 2.5× → 780×690. Use 20 for pixel-exact 2× (624×552). */
#define TELETEXT_SCALE_X10 25
/** Height of the status line above the keypad. */
#define STATUS_HEIGHT 110
#define PANEL_PAD 10

#define COLOR_SCREEN_BG   0x000000
#define COLOR_PANEL_BG    0x14171c
#define COLOR_KEY_BG      0x2b303a
#define COLOR_KEY_PRESSED 0x0050c0
#define COLOR_PAGE        0xffff00 /* teletext yellow */
#define COLOR_SUBPAGE     0xc0c0c0
#define COLOR_ENTRY       0x00ffff /* teletext cyan */

static lv_obj_t *s_img;
static lv_obj_t *s_img_note;
static lv_obj_t *s_net_note;
static lv_timer_t *s_net_note_timer;
static lv_obj_t *s_status_page;
static lv_obj_t *s_status_sub;
static lv_obj_t *s_status_entry;
static lv_obj_t *s_btnm;
static uint16_t *s_buf;
static uint16_t s_w;
static uint16_t s_h;
static lv_image_dsc_t s_dsc;

static teletext_catalog_t *s_cat;
static size_t s_cat_idx;
static uint8_t s_part;
static char s_digits[5];
static uint8_t s_digit_len;

/* Button ids follow the order of s_btnm_map (newlines are not buttons). */
enum {
    KEY_PAGE_PREV = 0,
    KEY_PART_PREV,
    KEY_PART_NEXT,
    KEY_PAGE_NEXT,
    KEY_1, KEY_2, KEY_3,
    KEY_4, KEY_5, KEY_6,
    KEY_7, KEY_8, KEY_9,
    KEY_FAV,
    KEY_0,
    KEY_DEL,
};

static const char *s_btnm_map[] = {
    LV_SYMBOL_PREV, LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT, LV_SYMBOL_NEXT, "\n",
    "1", "2", "3", "\n",
    "4", "5", "6", "\n",
    "7", "8", "9", "\n",
    "Fav", "0", LV_SYMBOL_BACKSPACE, "",
};

static bool catalog_ok(void)
{
    return s_cat != NULL && s_cat->entries != NULL && s_cat->count > 0 && s_cat_idx < s_cat->count;
}

/** Blue teletext-style test card, drawn at any size. */
static void draw_placeholder(uint16_t w, uint16_t h, uint16_t *px)
{
    const uint16_t bg = lv_color_to_u16(lv_color_make(0x00, 0x00, 0xC0));
    const uint16_t yellow = lv_color_to_u16(lv_color_make(0xFF, 0xFF, 0x00));
    const uint16_t white = lv_color_to_u16(lv_color_make(0xFF, 0xFF, 0xFF));
    const uint16_t red = lv_color_to_u16(lv_color_make(0xFF, 0x00, 0x00));

    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint16_t c = bg;
            if (y < h / 12) {
                c = yellow;
            } else if (y >= h / 6 && y < h * 5 / 12 && x >= w / 12 && x < w * 11 / 12) {
                c = white;
            } else if (y >= h / 2 && y < h * 3 / 4 && x >= w / 12 && x < w / 2) {
                c = red;
            }
            px[y * w + x] = c;
        }
    }
}

static void update_status_line(void)
{
    if (s_status_page == NULL) {
        return;
    }
    if (!catalog_ok()) {
        lv_label_set_text(s_status_page, "---");
        lv_label_set_text(s_status_sub, "no catalog");
    } else {
        const teletext_page_entry_t *e = &s_cat->entries[s_cat_idx];
        lv_label_set_text_fmt(s_status_page, "%u", e->page_id);
        if (e->num_parts > 1) {
            lv_label_set_text_fmt(s_status_sub, "%u/%u", (unsigned)s_part, (unsigned)e->num_parts);
        } else {
            lv_label_set_text(s_status_sub, "");
        }
    }
    /* Typed digits, padded with '-' to the usual three digits. */
    char entry[8];
    size_t n = 0;
    for (; n < s_digit_len && n < sizeof(entry) - 1; n++) {
        entry[n] = s_digits[n];
    }
    for (; n < 3; n++) {
        entry[n] = '-';
    }
    entry[n] = '\0';
    lv_label_set_text(s_status_entry, entry);
}

static void apply_current_page(void)
{
    if (s_img == NULL) {
        return; /* UI not built yet */
    }
    if (s_buf == NULL) {
        update_status_line();
        return;
    }

    if (!catalog_ok()) {
        draw_placeholder(s_w, s_h, s_buf);
        lv_label_set_text(s_img_note, "No pages yet - waiting for download");
        lv_obj_set_hidden(s_img_note, false);
    } else {
        const teletext_page_entry_t *e = &s_cat->entries[s_cat_idx];
        if (s_part < 1) {
            s_part = 1;
        }
        if (s_part > e->num_parts) {
            s_part = e->num_parts;
        }
        esp_err_t err = teletext_load_page_image(e->page_id, s_part, e->num_parts, s_buf, s_w, s_h);
        if (err != ESP_OK) {
            draw_placeholder(s_w, s_h, s_buf);
            lv_label_set_text_fmt(s_img_note, "Page %u not available", e->page_id);
            lv_obj_set_hidden(s_img_note, false);
        } else {
            lv_obj_set_hidden(s_img_note, true);
        }
    }

    /* LV_CACHE_DEF_SIZE is 0, so LVGL reads s_buf directly; a redraw is enough. */
    lv_obj_invalidate(s_img);
    update_status_line();
}

static void go_page(int delta)
{
    if (!catalog_ok()) {
        return;
    }
    if (delta < 0 && s_cat_idx > 0) {
        s_cat_idx--;
    } else if (delta > 0 && s_cat_idx + 1 < s_cat->count) {
        s_cat_idx++;
    } else {
        return;
    }
    s_part = 1;
    apply_current_page();
}

static void go_part(int delta)
{
    if (!catalog_ok()) {
        return;
    }
    const teletext_page_entry_t *e = &s_cat->entries[s_cat_idx];
    if (delta < 0 && s_part > 1) {
        s_part--;
    } else if (delta > 0 && s_part < e->num_parts) {
        s_part++;
    } else {
        return;
    }
    apply_current_page();
}

static void enter_digits(void)
{
    if (s_digit_len == 0) {
        return;
    }
    s_digits[s_digit_len] = '\0';
    int p = atoi(s_digits);
    s_digit_len = 0;
    s_digits[0] = '\0';
    if (catalog_ok()) {
        int idx = teletext_catalog_find(s_cat, (uint16_t)p);
        if (idx >= 0) {
            s_cat_idx = (size_t)idx;
            s_part = 1;
            apply_current_page();
            return;
        }
        ESP_LOGI(TAG, "Page %d not in catalog", p);
    }
    update_status_line();
}

static void keypad_event_cb(lv_event_t *ev)
{
    lv_obj_t *obj = lv_event_get_target_obj(ev);
    uint32_t id = lv_buttonmatrix_get_selected_button(obj);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) {
        return;
    }

    switch (id) {
    case KEY_PAGE_PREV: go_page(-1); break;
    case KEY_PAGE_NEXT: go_page(+1); break;
    case KEY_PART_PREV: go_part(-1); break;
    case KEY_PART_NEXT: go_part(+1); break;
    case KEY_DEL:
        if (s_digit_len > 0) {
            s_digits[--s_digit_len] = '\0';
            update_status_line();
        }
        break;
    case KEY_FAV: ESP_LOGI(TAG, "fav (not implemented)"); break;
    default: {
        const char *txt = lv_buttonmatrix_get_button_text(obj, id);
        if (txt != NULL && txt[0] >= '0' && txt[0] <= '9' && txt[1] == '\0' &&
            s_digit_len + 1 < sizeof(s_digits)) {
            s_digits[s_digit_len++] = txt[0];
            s_digits[s_digit_len] = '\0';
            update_status_line();
            /* Teletext pages have three digits: jump as soon as the third is typed. */
            if (s_digit_len == 3) {
                enter_digits();
            }
        }
        break;
    }
    }
}

static void picture_gesture_cb(lv_event_t *ev)
{
    (void)ev;
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }
    switch (lv_indev_get_gesture_dir(indev)) {
    case LV_DIR_LEFT:  go_part(+1); break;
    case LV_DIR_RIGHT: go_part(-1); break;
    case LV_DIR_TOP:   go_page(+1); break;
    case LV_DIR_BOTTOM: go_page(-1); break;
    default: break;
    }
}

static lv_obj_t *make_panel(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t bg)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(o, false);
    return o;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

void teletext_ui_init(lv_obj_t *scr, teletext_catalog_t *cat)
{
    s_cat = cat;
    s_part = 1;
    s_digit_len = 0;
    s_digits[0] = '\0';

    /* Start on teletext page 100 (if present in catalog). */
    s_cat_idx = 0;
    if (s_cat != NULL && s_cat->entries != NULL) {
        int idx = teletext_catalog_find(s_cat, 100);
        s_cat_idx = (idx >= 0) ? (size_t)idx : 0;
    }

    lv_display_t *disp = lv_obj_get_display(scr);
    const int32_t scr_w = lv_display_get_horizontal_resolution(disp);
    const int32_t scr_h = lv_display_get_vertical_resolution(disp);

    /* Picture size: scaled source, limited to the screen height. */
    int32_t img_w = TELETEXT_SRC_W * TELETEXT_SCALE_X10 / 10;
    int32_t img_h = TELETEXT_SRC_H * TELETEXT_SCALE_X10 / 10;
    if (img_h > scr_h) {
        img_w = img_w * scr_h / img_h;
        img_h = scr_h;
    }
    s_w = (uint16_t)img_w;
    s_h = (uint16_t)img_h;
    const int32_t keys_x = img_w;
    const int32_t keys_w = scr_w - img_w;
    ESP_LOGI(TAG, "Screen %ldx%ld, picture %ux%u, keypad %ld px wide", (long)scr_w, (long)scr_h, s_w, s_h,
             (long)keys_w);

    const size_t buf_bytes = (size_t)s_w * s_h * sizeof(uint16_t);
    s_buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_buf == NULL) {
        ESP_LOGE(TAG, "PSRAM alloc failed for %zu bytes", buf_bytes);
    }

    memset(&s_dsc, 0, sizeof(s_dsc));
    s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_dsc.header.w = s_w;
    s_dsc.header.h = s_h;
    s_dsc.header.stride = s_w * (uint32_t)sizeof(uint16_t);
    s_dsc.data_size = buf_bytes;
    s_dsc.data = (const uint8_t *)s_buf;

    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_SCREEN_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_scrollable(scr, false);

    /* Left: picture panel (full height, picture centred vertically). Swipes navigate. */
    lv_obj_t *pic = make_panel(scr, 0, 0, img_w, scr_h, COLOR_SCREEN_BG);
    lv_obj_add_event_cb(pic, picture_gesture_cb, LV_EVENT_GESTURE, NULL);

    s_img = lv_image_create(pic);
    lv_obj_set_clickable(s_img, false);
    if (s_buf != NULL) {
        lv_image_set_src(s_img, &s_dsc);
    }
    lv_obj_center(s_img);

    s_img_note = make_label(pic, &lv_font_montserrat_28, 0xffffff);
    lv_obj_set_style_bg_color(s_img_note, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_img_note, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(s_img_note, 12, 0);
    lv_obj_align(s_img_note, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_hidden(s_img_note, true);

    /* Wi-Fi / download status, top right of the picture. */
    s_net_note = make_label(pic, &lv_font_montserrat_28, 0xffffff);
    lv_obj_set_style_bg_color(s_net_note, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_net_note, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(s_net_note, 8, 0);
    lv_obj_set_style_radius(s_net_note, 6, 0);
    lv_obj_align(s_net_note, LV_ALIGN_TOP_RIGHT, -20, 24);
    lv_obj_set_hidden(s_net_note, true);

    /* Right: status line + keypad. */
    lv_obj_t *side = make_panel(scr, keys_x, 0, keys_w, scr_h, COLOR_PANEL_BG);

    lv_obj_t *status = make_panel(side, PANEL_PAD, PANEL_PAD, keys_w - 2 * PANEL_PAD, STATUS_HEIGHT,
                                  COLOR_SCREEN_BG);
    lv_obj_set_style_radius(status, 10, 0);
    lv_obj_set_style_pad_hor(status, 16, 0);

    s_status_page = make_label(status, &lv_font_montserrat_48, COLOR_PAGE);
    lv_obj_align(s_status_page, LV_ALIGN_LEFT_MID, 0, 0);

    s_status_sub = make_label(status, &lv_font_montserrat_40, COLOR_SUBPAGE);
    lv_obj_align(s_status_sub, LV_ALIGN_CENTER, 0, 0);

    s_status_entry = make_label(status, &lv_font_montserrat_48, COLOR_ENTRY);
    lv_obj_align(s_status_entry, LV_ALIGN_RIGHT_MID, 0, 0);

    const int32_t keys_y = PANEL_PAD + STATUS_HEIGHT + PANEL_PAD;
    s_btnm = lv_buttonmatrix_create(side);
    lv_buttonmatrix_set_map(s_btnm, s_btnm_map);
    lv_obj_set_pos(s_btnm, PANEL_PAD, keys_y);
    lv_obj_set_size(s_btnm, keys_w - 2 * PANEL_PAD, scr_h - keys_y - PANEL_PAD);
    lv_obj_set_style_bg_opa(s_btnm, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_btnm, 0, 0);
    lv_obj_set_style_pad_all(s_btnm, 0, 0);
    lv_obj_set_style_pad_row(s_btnm, 10, 0);
    lv_obj_set_style_pad_column(s_btnm, 10, 0);
    lv_obj_set_style_bg_color(s_btnm, lv_color_hex(COLOR_KEY_BG), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_btnm, lv_color_hex(COLOR_KEY_PRESSED), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(s_btnm, lv_color_hex(0xffffff), LV_PART_ITEMS);
    lv_obj_set_style_text_font(s_btnm, &lv_font_montserrat_40, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_btnm, 10, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_btnm, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(s_btnm, 0, LV_PART_ITEMS);
    lv_obj_add_event_cb(s_btnm, keypad_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    apply_current_page();
}

teletext_catalog_t *teletext_ui_set_catalog(teletext_catalog_t *cat)
{
    teletext_catalog_t *old = s_cat;
    const bool had_page = catalog_ok();
    const uint16_t page = had_page ? s_cat->entries[s_cat_idx].page_id : 100;

    s_cat = cat;
    s_cat_idx = 0;
    if (s_cat != NULL && s_cat->entries != NULL) {
        int idx = teletext_catalog_find(s_cat, page);
        if (idx < 0) {
            idx = teletext_catalog_find(s_cat, 100);
            s_part = 1;
        }
        s_cat_idx = (idx >= 0) ? (size_t)idx : 0;
    }
    if (!had_page) {
        s_part = 1;
    }
    /* apply_current_page() clamps s_part to the new subpage count. */
    apply_current_page();
    return old;
}

static void net_note_hide_cb(lv_timer_t *t)
{
    (void)t;
    s_net_note_timer = NULL;
    if (s_net_note != NULL) {
        lv_obj_set_hidden(s_net_note, true);
    }
}

void teletext_ui_set_notice(const char *text, uint32_t hide_after_ms)
{
    if (s_net_note == NULL) {
        return;
    }
    if (s_net_note_timer != NULL) {
        lv_timer_delete(s_net_note_timer);
        s_net_note_timer = NULL;
    }
    if (text == NULL || text[0] == '\0') {
        lv_obj_set_hidden(s_net_note, true);
        return;
    }
    lv_label_set_text(s_net_note, text);
    lv_obj_set_hidden(s_net_note, false);
    if (hide_after_ms > 0) {
        s_net_note_timer = lv_timer_create(net_note_hide_cb, hide_after_ms, NULL);
        lv_timer_set_repeat_count(s_net_note_timer, 1);
    }
}
