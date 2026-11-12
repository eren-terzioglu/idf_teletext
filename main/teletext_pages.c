#include "teletext_pages.h"

#include "esp_heap_caps.h"

void teletext_catalog_destroy(teletext_catalog_t *cat)
{
    if (cat == NULL) {
        return;
    }
    heap_caps_free(cat->entries);
    heap_caps_free(cat);
}

int teletext_catalog_find(const teletext_catalog_t *cat, uint16_t page_id)
{
    if (cat == NULL || cat->entries == NULL) {
        return -1;
    }
    /* Entries are sorted by page_id. */
    size_t lo = 0, hi = cat->count;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        const uint16_t p = cat->entries[mid].page_id;
        if (p == page_id) {
            return (int)mid;
        }
        if (p < page_id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return -1;
}
