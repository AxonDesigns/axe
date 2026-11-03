// CPU-side cache of rasterized glyph bitmaps. Worker threads that prepare text rasterize
// into it; the atlas copies a bitmap from it into a page the first time a glyph is drawn,
// instead of rasterizing on the core thread.
//
// Thread-safe (one lock per shard). Bounded by bytes: least recently used entries are
// evicted first, except entries protected by a preparation the core hasn't released yet
// (a sequence number above the released one), so prepared glyphs survive until drawn.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "text/font_registry.h"
#include "text/raster.h"

typedef struct ui_glyph_key {
    ui_font_key font;
    uint32_t glyph;
    uint32_t bin;      // horizontal subpixel bin, 0..UI_ATLAS_SUBPIXEL_BINS-1
} ui_glyph_key;

typedef struct ui_glyph_cache ui_glyph_cache;

typedef struct ui_glyph_cache_stats {
    uint32_t entries;
    uint64_t bytes;
    uint64_t stored;   // entries ever added
    uint64_t evicted;
} ui_glyph_cache_stats;

#define UI_GLYPH_CACHE_DEFAULT_BYTES (16u << 20)

// budget_bytes 0 = UI_GLYPH_CACHE_DEFAULT_BYTES.
ui_result ui_glyph_cache_create(size_t budget_bytes, ui_glyph_cache **out);
void ui_glyph_cache_destroy(ui_glyph_cache *c);

// If `key` is cached, raises its protection to `seq` and returns true. Thread: any.
bool ui_glyph_cache_protect(ui_glyph_cache *c, ui_glyph_key key, uint64_t seq);
// Stores a copy of `bm` (NULL: the glyph could not be rasterized, which is cached too, so
// the core doesn't retry it), protected until `seq` is released. An entry that already
// exists only has its protection raised. Thread: any. Fails only on allocation.
ui_result ui_glyph_cache_put(ui_glyph_cache *c, ui_glyph_key key, const ui_bitmap *bm,
                             uint64_t seq);
// Calls fn with the cached bitmap (bm NULL for a glyph that could not be rasterized) while
// holding the entry's lock, and marks it used. False if the key isn't cached. Thread: any.
bool ui_glyph_cache_with(ui_glyph_cache *c, ui_glyph_key key,
                         void (*fn)(void *user, const ui_bitmap *bm), void *user);
// Entries protected by sequence numbers up to `seq` may be evicted again. Thread: core.
void ui_glyph_cache_release(ui_glyph_cache *c, uint64_t seq);

void ui_glyph_cache_get_stats(ui_glyph_cache *c, ui_glyph_cache_stats *out);
