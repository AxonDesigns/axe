#include "text/atlas.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/common.h"

#define SHELF_ROUND 4u

typedef struct span {
    uint16_t x, w;
} span;

typedef struct shelf {
    uint16_t y, h;
    uint16_t bump;    // first never-used column
    uint32_t live;    // glyphs in this shelf
    span *free;       // freed column ranges, sorted by x, merged
    uint32_t free_count, free_capacity;
} shelf;

typedef struct page {
    bool live;
    uint32_t glyphs;
    uint64_t empty_since; // epoch the page last became empty
    shelf *shelves;       // sorted by y
    uint32_t shelf_count, shelf_capacity;
    uint32_t next_y;      // top of the unused area
} page;

enum { SLOT_EMPTY = 0, SLOT_LIVE, SLOT_TOMBSTONE };

typedef struct lru_entry {
    uint64_t last_used;
    uint32_t slot;
} lru_entry;

typedef struct entry {
    ui_glyph_key key;
    ui_atlas_glyph glyph;
    uint64_t last_used;
    uint8_t state;
    bool placed;      // occupies atlas space
} entry;

struct ui_atlas {
    uint32_t page_size, max_pages, evict_frames;
    page pages[UI_ATLAS_MAX_PAGES];
    entry *slots;
    uint32_t capacity, live, tombstones; // capacity is a power of two
    uint64_t epoch, protect_epoch, seq;
    // Pending update.
    ui_atlas_page_op *ops;
    uint32_t op_count, op_capacity;
    ui_atlas_write *writes;
    uint32_t write_count, write_capacity;
    uint8_t *pixels;
    uint32_t pixel_count, pixel_capacity;
    lru_entry *lru; // scratch for pressure eviction
    uint32_t lru_capacity;
    ui_atlas_stats stats;
};

ui_result ui_atlas_create(const ui_atlas_desc *desc, ui_atlas **out) {
    ui_atlas *a = calloc(1, sizeof *a);
    if (!a) return UI_ERR_OUT_OF_MEMORY;
    a->page_size = desc->page_size ? desc->page_size : 1024;
    a->max_pages = desc->max_pages ? desc->max_pages : 8;
    if (a->max_pages > UI_ATLAS_MAX_PAGES) a->max_pages = UI_ATLAS_MAX_PAGES;
    if (a->page_size > UINT16_MAX || a->page_size < 64) {
        free(a);
        return UI_ERR_INVALID_ARGUMENT;
    }
    a->evict_frames = desc->evict_frames ? desc->evict_frames : 300;
    a->capacity = 1024;
    a->slots = calloc(a->capacity, sizeof *a->slots);
    if (!a->slots) {
        free(a);
        return UI_ERR_OUT_OF_MEMORY;
    }
    a->protect_epoch = UINT64_MAX;
    *out = a;
    return UI_OK;
}

void ui_atlas_destroy(ui_atlas *a) {
    if (!a) return;
    for (uint32_t p = 0; p < UI_ATLAS_MAX_PAGES; p++) {
        for (uint32_t s = 0; s < a->pages[p].shelf_count; s++) free(a->pages[p].shelves[s].free);
        free(a->pages[p].shelves);
    }
    free(a->slots);
    free(a->lru);
    free(a->ops);
    free(a->writes);
    free(a->pixels);
    free(a);
}

// ---- Hash table --------------------------------------------------------------------------

static uint32_t hash_key(ui_glyph_key k) {
    uint64_t h = 0x9E3779B97F4A7C15ull;
    uint64_t parts[] = {k.font.face, k.font.instance, k.font.size_q, k.glyph, k.bin};
    for (size_t i = 0; i < UI_ARRAY_LEN(parts); i++) {
        h ^= parts[i] + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        h *= 0xBF58476D1CE4E5B9ull;
    }
    return (uint32_t)(h ^ (h >> 32));
}

static bool key_eq(ui_glyph_key a, ui_glyph_key b) {
    return a.font.face == b.font.face && a.font.instance == b.font.instance &&
           a.font.size_q == b.font.size_q && a.glyph == b.glyph && a.bin == b.bin;
}

// The live slot for key, or (if absent) the slot to insert into. *found tells which.
static entry *probe(ui_atlas *a, ui_glyph_key key, bool *found) {
    uint32_t mask = a->capacity - 1;
    entry *tomb = NULL;
    for (uint32_t i = hash_key(key) & mask;; i = (i + 1) & mask) {
        entry *e = &a->slots[i];
        if (e->state == SLOT_EMPTY) {
            *found = false;
            return tomb ? tomb : e;
        }
        if (e->state == SLOT_TOMBSTONE) {
            if (!tomb) tomb = e;
        } else if (key_eq(e->key, key)) {
            *found = true;
            return e;
        }
    }
}

static ui_result rehash(ui_atlas *a, uint32_t capacity) {
    entry *old = a->slots;
    uint32_t old_cap = a->capacity;
    entry *slots = calloc(capacity, sizeof *slots);
    if (!slots) return UI_ERR_OUT_OF_MEMORY;
    a->slots = slots;
    a->capacity = capacity;
    a->tombstones = 0;
    for (uint32_t i = 0; i < old_cap; i++) {
        if (old[i].state != SLOT_LIVE) continue;
        bool found;
        *probe(a, old[i].key, &found) = old[i];
    }
    free(old);
    return UI_OK;
}

// ---- Packing -----------------------------------------------------------------------------

static void free_in_shelf(page *pg, uint32_t shelf_index, uint16_t x, uint16_t w) {
    shelf *s = &pg->shelves[shelf_index];
    s->live--;
    if (s->live == 0) {
        // An empty shelf forgets its columns; a trailing one gives its rows back.
        s->bump = 0;
        s->free_count = 0;
        while (pg->shelf_count && pg->shelves[pg->shelf_count - 1].live == 0 &&
               pg->shelves[pg->shelf_count - 1].bump == 0) {
            shelf *last = &pg->shelves[pg->shelf_count - 1];
            pg->next_y = last->y;
            free(last->free);
            pg->shelf_count--;
        }
        return;
    }
    // Insert and merge with neighbours (the list stays sorted by x).
    uint32_t i = 0;
    while (i < s->free_count && s->free[i].x < x) i++;
    bool merged = false;
    if (i > 0 && s->free[i - 1].x + s->free[i - 1].w == x) {
        s->free[i - 1].w += w;
        merged = true;
        i--;
    }
    if (!merged) {
        if (ui_grow((void **)&s->free, &s->free_capacity, s->free_count + 1, sizeof *s->free) != UI_OK)
            return; // the columns are lost until the shelf empties: no harm beyond waste
        memmove(&s->free[i + 1], &s->free[i], (s->free_count - i) * sizeof *s->free);
        s->free[i] = (span){x, w};
        s->free_count++;
    }
    if (i + 1 < s->free_count && s->free[i].x + s->free[i].w == s->free[i + 1].x) {
        s->free[i].w += s->free[i + 1].w;
        memmove(&s->free[i + 1], &s->free[i + 2], (s->free_count - i - 2) * sizeof *s->free);
        s->free_count--;
    }
    // Free space touching the bump pointer returns to it.
    if (s->free_count && s->free[s->free_count - 1].x + s->free[s->free_count - 1].w == s->bump) {
        s->bump = s->free[s->free_count - 1].x;
        s->free_count--;
    }
}

static bool alloc_in_shelf(ui_atlas *a, shelf *s, uint16_t w, uint16_t *x) {
    for (uint32_t i = 0; i < s->free_count; i++) {
        if (s->free[i].w < w) continue;
        *x = s->free[i].x;
        s->free[i].x = (uint16_t)(s->free[i].x + w);
        s->free[i].w = (uint16_t)(s->free[i].w - w);
        if (!s->free[i].w) {
            memmove(&s->free[i], &s->free[i + 1], (s->free_count - i - 1) * sizeof *s->free);
            s->free_count--;
        }
        s->live++;
        return true;
    }
    if (s->bump + w > a->page_size) return false;
    *x = s->bump;
    s->bump = (uint16_t)(s->bump + w);
    s->live++;
    return true;
}

static bool alloc_in_page(ui_atlas *a, page *pg, uint16_t w, uint16_t h, uint16_t *x,
                          uint16_t *y) {
    uint16_t want = (uint16_t)((h + SHELF_ROUND - 1) / SHELF_ROUND * SHELF_ROUND);
    uint16_t slack = (uint16_t)(want / 4 > SHELF_ROUND ? want / 4 : SHELF_ROUND);
    // Best-fitting existing shelf: tallest enough, wasting the least height.
    shelf *best = NULL;
    for (uint32_t i = 0; i < pg->shelf_count; i++) {
        shelf *s = &pg->shelves[i];
        if (s->h < want || s->h > want + slack) continue;
        if (best && s->h >= best->h) continue;
        if (s->bump + w <= a->page_size) {
            best = s;
            continue;
        }
        for (uint32_t k = 0; k < s->free_count; k++) {
            if (s->free[k].w >= w) {
                best = s;
                break;
            }
        }
    }
    if (best && alloc_in_shelf(a, best, w, x)) {
        *y = best->y;
        return true;
    }
    if (pg->next_y + want > a->page_size) return false;
    if (ui_grow((void **)&pg->shelves, &pg->shelf_capacity, pg->shelf_count + 1,
                sizeof *pg->shelves) != UI_OK)
        return false;
    shelf *s = &pg->shelves[pg->shelf_count++];
    *s = (shelf){.y = (uint16_t)pg->next_y, .h = want};
    pg->next_y += want;
    alloc_in_shelf(a, s, w, x);
    *y = s->y;
    return true;
}

static uint32_t shelf_at(const page *pg, uint16_t y) {
    for (uint32_t i = 0; i < pg->shelf_count; i++) {
        if (pg->shelves[i].y == y) return i;
    }
    return UINT32_MAX;
}

static bool push_page_op(ui_atlas *a, ui_atlas_page_op op) {
    if (ui_grow((void **)&a->ops, &a->op_capacity, a->op_count + 1, sizeof *a->ops) != UI_OK)
        return false;
    a->ops[a->op_count++] = op;
    return true;
}

static void release_entry(ui_atlas *a, entry *e) {
    if (e->placed) {
        page *pg = &a->pages[e->glyph.page];
        uint32_t si = shelf_at(pg, e->glyph.y);
        if (si != UINT32_MAX) free_in_shelf(pg, si, e->glyph.x, e->glyph.width);
        if (--pg->glyphs == 0) pg->empty_since = a->epoch;
    }
    e->state = SLOT_TOMBSTONE;
    e->placed = false;
    a->live--;
    a->tombstones++;
    a->stats.evicted++;
}

static bool evictable(const ui_atlas *a, const entry *e, bool pressure) {
    if (e->state != SLOT_LIVE) return false;
    if (e->last_used >= a->protect_epoch || e->last_used >= a->epoch) return false;
    return pressure || e->last_used + a->evict_frames <= a->epoch;
}

static bool try_place(ui_atlas *a, uint16_t w, uint16_t h, uint16_t *pg_out, uint16_t *x,
                      uint16_t *y) {
    for (uint32_t p = 0; p < a->max_pages; p++) {
        if (a->pages[p].live && alloc_in_page(a, &a->pages[p], w, h, x, y)) {
            *pg_out = (uint16_t)p;
            return true;
        }
    }
    for (uint32_t p = 0; p < a->max_pages; p++) {
        if (a->pages[p].live) continue;
        if (!push_page_op(a, (ui_atlas_page_op){(uint16_t)p, UI_ATLAS_R8, a->page_size}))
            return false;
        a->pages[p].live = true;
        a->pages[p].next_y = 0;
        a->stats.pages++;
        if (alloc_in_page(a, &a->pages[p], w, h, x, y)) {
            *pg_out = (uint16_t)p;
            return true;
        }
    }
    return false;
}

static int lru_order(const void *x, const void *y) {
    const lru_entry *a = x, *b = y;
    return a->last_used < b->last_used ? -1 : a->last_used > b->last_used;
}

static bool place(ui_atlas *a, uint16_t w, uint16_t h, uint16_t *pg_out, uint16_t *x, uint16_t *y) {
    if (try_place(a, w, h, pg_out, x, y)) return true;
    // Pressure: evict glyphs not used this frame and not protected, least recently used
    // first, in batches, until the new one fits.
    uint32_t n = 0;
    for (uint32_t i = 0; i < a->capacity; i++) {
        if (!evictable(a, &a->slots[i], true)) continue;
        if (ui_grow((void **)&a->lru, &a->lru_capacity, n + 1, sizeof *a->lru) != UI_OK) break;
        a->lru[n++] = (lru_entry){a->slots[i].last_used, i};
    }
    if (n) qsort(a->lru, n, sizeof *a->lru, lru_order); // lru is NULL until first used
    uint32_t batch = n / 8 > 8 ? n / 8 : 8;
    for (uint32_t k = 0; k < n;) {
        for (uint32_t end = k + batch < n ? k + batch : n; k < end; k++)
            release_entry(a, &a->slots[a->lru[k].slot]);
        if (try_place(a, w, h, pg_out, x, y)) return true;
    }
    return false;
}

// ---- Public ------------------------------------------------------------------------------

void ui_atlas_begin_frame(ui_atlas *a, uint64_t epoch, uint64_t protect_epoch) {
    a->epoch = epoch;
    a->protect_epoch = protect_epoch;
}

const ui_atlas_glyph *ui_atlas_get(ui_atlas *a, ui_raster *raster, ui_font *font,
                                   ui_glyph_key key) {
    bool found;
    entry *e = probe(a, key, &found);
    if (found) {
        e->last_used = a->epoch;
        return &e->glyph;
    }
    if ((a->live + a->tombstones + 1) * 4 > a->capacity * 3) {
        uint32_t cap = a->live * 2 + 2 > a->capacity / 2 ? a->capacity * 2 : a->capacity;
        if (rehash(a, cap) != UI_OK) return NULL;
        e = probe(a, key, &found);
    }
    ui_bitmap bm;
    float dx = (float)key.bin / UI_ATLAS_SUBPIXEL_BINS;
    ui_result res = ui_raster_glyph(raster, font, key.glyph, dx, &bm);
    if (res == UI_ERR_OUT_OF_MEMORY) return NULL;
    a->stats.rasterized++;
    entry fresh = {.key = key, .last_used = a->epoch, .state = SLOT_LIVE};
    uint32_t limit = a->page_size; // a glyph may fill a page; larger ones need bigger pages
    if (res != UI_OK || bm.width > limit || bm.height > limit) {
        a->stats.rejected++;
    } else if (bm.width && bm.height) {
        uint16_t pg, x, y;
        if (!place(a, (uint16_t)bm.width, (uint16_t)bm.height, &pg, &x, &y)) {
            a->stats.rejected++;
        } else if (ui_grow((void **)&a->writes, &a->write_capacity, a->write_count + 1,
                           sizeof *a->writes) != UI_OK ||
                   ui_grow((void **)&a->pixels, &a->pixel_capacity,
                           a->pixel_count + bm.width * bm.height, 1) != UI_OK) {
            free_in_shelf(&a->pages[pg], shelf_at(&a->pages[pg], y), x, (uint16_t)bm.width);
            return NULL;
        } else {
            fresh.glyph = (ui_atlas_glyph){pg, x, y, (uint16_t)bm.width, (uint16_t)bm.height,
                                           (int16_t)bm.left, (int16_t)bm.top};
            fresh.placed = true;
            a->pages[pg].glyphs++;
            a->writes[a->write_count++] = (ui_atlas_write){pg, x, y, (uint16_t)bm.width,
                                                           (uint16_t)bm.height, a->pixel_count};
            memcpy(a->pixels + a->pixel_count, bm.pixels, (size_t)bm.width * bm.height);
            a->pixel_count += bm.width * bm.height;
        }
    }
    // place() may have evicted under pressure; the probe slot stays valid (no rehash), but a
    // tombstone it created earlier in the chain is as good: probe again.
    e = probe(a, key, &found);
    if (e->state == SLOT_TOMBSTONE) a->tombstones--;
    *e = fresh;
    a->live++;
    return &e->glyph;
}

void ui_atlas_end_frame(ui_atlas *a) {
    for (uint32_t i = 0; i < a->capacity; i++) {
        if (evictable(a, &a->slots[i], false)) release_entry(a, &a->slots[i]);
    }
    // Pages after the first that stayed empty for the eviction window go back to the GPU.
    for (uint32_t p = 1; p < a->max_pages; p++) {
        page *pg = &a->pages[p];
        if (!pg->live || pg->glyphs || pg->empty_since + a->evict_frames > a->epoch) continue;
        if (!push_page_op(a, (ui_atlas_page_op){(uint16_t)p, UI_ATLAS_R8, 0})) continue;
        for (uint32_t s = 0; s < pg->shelf_count; s++) free(pg->shelves[s].free);
        free(pg->shelves);
        *pg = (page){0};
        a->stats.pages--;
    }
}

ui_atlas_update *ui_atlas_take_update(ui_atlas *a, bool *failed) {
    *failed = false;
    if (!a->op_count && !a->write_count) return NULL;
    size_t ops = (size_t)a->op_count * sizeof *a->ops;
    size_t writes = (size_t)a->write_count * sizeof *a->writes;
    ui_atlas_update *u = malloc(sizeof *u + ops + writes + a->pixel_count);
    if (!u) {
        *failed = true;
        return NULL;
    }
    uint8_t *p = (uint8_t *)(u + 1);
    *u = (ui_atlas_update){
        .seq = ++a->seq,
        .page_op_count = a->op_count,
        .write_count = a->write_count,
        .pixel_bytes = a->pixel_count,
        .page_ops = (ui_atlas_page_op *)p,
        .writes = (ui_atlas_write *)(p + ops),
        .pixels = p + ops + writes,
    };
    memcpy(u->page_ops, a->ops, ops);
    memcpy(u->writes, a->writes, writes);
    memcpy(u->pixels, a->pixels, a->pixel_count);
    a->op_count = a->write_count = a->pixel_count = 0;
    a->stats.updates++;
    return u;
}

uint64_t ui_atlas_seq(const ui_atlas *a) { return a->seq; }

void ui_atlas_get_stats(const ui_atlas *a, ui_atlas_stats *out) {
    *out = a->stats;
    out->glyphs = a->live;
}

uint32_t ui_atlas_page_size(const ui_atlas *a) { return a->page_size; }
