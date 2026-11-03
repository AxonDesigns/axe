// Glyph atlas, owned by the core: rasterizes glyphs on demand, packs them into pages, evicts
// glyphs that went unused, and records every change in an update that the render thread
// applies. The render thread never rasterizes or allocates atlas space; it only uploads.
//
// Upload protocol (docs/text.md has the full argument):
//  - Each frame's changes go out as one ui_atlas_update with an increasing `seq`, through the
//    render actor's unbounded FIFO inbox. Display lists go through the mailbox, which may
//    replace an undrawn list; the inbox never drops or replaces anything.
//  - The core pushes the update before publishing any list built in that frame and stamps
//    each list with the newest seq pushed. The render actor applies updates in order, only
//    between lists, and draws a list only once it has applied that list's seq. An update for
//    a list that was replaced is therefore still applied before the next list.
//  - A glyph is evicted only when no surface's newest unfinished list uses it (the caller
//    passes that bound), so no list still to be drawn reads overwritten texels.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "text/font_registry.h"
#include "text/glyph_cache.h"
#include "text/raster.h"

#define UI_ATLAS_SUBPIXEL_BINS 4
#define UI_ATLAS_MAX_PAGES 64

typedef enum ui_atlas_format {
    UI_ATLAS_R8 = 0,   // coverage
    UI_ATLAS_RGBA8,    // reserved for color glyphs
} ui_atlas_format;

// A cached glyph. width or height 0: nothing to draw (spaces, rejected glyphs).
typedef struct ui_atlas_glyph {
    uint16_t page, x, y, width, height;
    int16_t left, top;  // bitmap offset from the pen position, px (top is y down)
} ui_atlas_glyph;

typedef struct ui_atlas_desc {
    uint32_t page_size;    // px per side; 0 = 1024
    uint32_t max_pages;    // 0 = 8, at most UI_ATLAS_MAX_PAGES
    uint32_t evict_frames; // epochs a glyph may go unused before eviction; 0 = 300
} ui_atlas_desc;

typedef struct ui_atlas_stats {
    uint32_t glyphs;       // cached now
    uint32_t pages;        // allocated now
    uint64_t rasterized;   // glyphs added (rasterized here or copied from the glyph cache)
    uint64_t from_cache;   // of those, copied from the glyph cache (prepared by a worker)
    uint64_t evicted;
    uint64_t rejected;     // larger than a page, or no space left
    uint64_t updates;      // updates produced
} ui_atlas_stats;

// What the render thread applies, in order: page creations and releases, then rectangle
// writes. One malloc'd block; ownership moves with the message.
typedef struct ui_atlas_page_op {
    uint16_t page;
    uint16_t format;   // ui_atlas_format; ignored for releases
    uint32_t size;     // px per side; 0 releases the page
} ui_atlas_page_op;

typedef struct ui_atlas_write {
    uint16_t page, x, y, width, height;
    uint32_t offset;   // into pixels; tightly packed rows of `width` bytes
} ui_atlas_write;

typedef struct ui_atlas_update {
    uint64_t seq;
    uint32_t page_op_count, write_count;
    uint32_t pixel_bytes;
    ui_atlas_page_op *page_ops;
    ui_atlas_write *writes;
    uint8_t *pixels;
} ui_atlas_update;

typedef struct ui_atlas ui_atlas;

ui_result ui_atlas_create(const ui_atlas_desc *desc, ui_atlas **out);
void ui_atlas_destroy(ui_atlas *a);

// Starts a frame. `epoch` increases by one per drawn core frame. Glyphs last used at or
// after `protect_epoch` are never evicted (the oldest epoch among surfaces' newest
// unfinished display lists; UINT64_MAX if none is in flight).
void ui_atlas_begin_frame(ui_atlas *a, uint64_t epoch, uint64_t protect_epoch);

// Glyphs missing from the atlas are looked up in `cache` (may be NULL) before being
// rasterized here. The cache must outlive the atlas.
void ui_atlas_set_glyph_cache(ui_atlas *a, ui_glyph_cache *cache);

// The glyph for `key`, packing it on first use (from the glyph cache, else rasterized now),
// and marks it used this epoch. NULL on allocation failure; a glyph that cannot be placed
// is cached as empty (and counted as rejected) so it is not retried every frame.
const ui_atlas_glyph *ui_atlas_get(ui_atlas *a, ui_raster *raster, ui_font *font,
                                   ui_glyph_key key);

// Ends the frame: evicts glyphs unused for evict_frames (subject to protect_epoch) and
// releases trailing pages that became empty.
void ui_atlas_end_frame(ui_atlas *a);

// The changes since the last take as one update (seq increments), or NULL when nothing
// changed. Ownership moves to the caller (free with free()). On allocation failure the
// changes stay pending and NULL is returned with *failed set.
ui_atlas_update *ui_atlas_take_update(ui_atlas *a, bool *failed);
// The newest seq handed out (0 before the first update).
uint64_t ui_atlas_seq(const ui_atlas *a);

void ui_atlas_get_stats(const ui_atlas *a, ui_atlas_stats *out);
uint32_t ui_atlas_page_size(const ui_atlas *a);
