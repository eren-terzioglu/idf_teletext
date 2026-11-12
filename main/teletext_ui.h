#pragma once

#include "lvgl.h"
#include "teletext_pages.h"

/**
 * Landscape teletext UI for the Tab5 (1280×720):
 *   left  — teletext picture, scaled from 312×276 into a PSRAM RGB565 buffer;
 *   right — status line and keypad; the third typed digit jumps (no Enter key).
 * Swipes on the picture: left/right = next/previous subpage, up/down = next/previous page.
 *
 * @param cat initial catalog, or NULL (pages arrive later via teletext_ui_set_catalog()).
 */
void teletext_ui_init(lv_obj_t *scr, teletext_catalog_t *cat);

/**
 * Switch to another catalog (e.g. a freshly downloaded one) and redraw the current page.
 * Stays on the same page number when the new catalog has it. Caller holds the display lock.
 *
 * @return the previous catalog pointer, which the caller may free once this returns.
 */
teletext_catalog_t *teletext_ui_set_catalog(teletext_catalog_t *cat);

/**
 * Show a short status note at the top of the picture (Wi-Fi / download progress).
 * @a hide_after_ms = 0 keeps it visible; NULL or "" hides it. Caller holds the display lock.
 */
void teletext_ui_set_notice(const char *text, uint32_t hide_after_ms);
