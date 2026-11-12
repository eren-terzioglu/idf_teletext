#pragma once

#include <stddef.h>
#include <stdint.h>

/** One logical teletext page: page number and its number of subpage pictures. */
typedef struct {
    uint16_t page_id;
    uint8_t num_parts;
} teletext_page_entry_t;

/** All pages, sorted by page_id (built from the downloaded index or the saved cache). */
typedef struct {
    teletext_page_entry_t *entries;
    size_t count;
} teletext_catalog_t;

/** Free a catalog allocated by teletext_online / teletext_cache (entries and the struct). */
void teletext_catalog_destroy(teletext_catalog_t *cat);

/** Index in catalog or -1 if not found. */
int teletext_catalog_find(const teletext_catalog_t *cat, uint16_t page_id);
