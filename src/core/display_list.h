// Renderer-agnostic display list: what one surface shows in one frame. Built by the core,
// published through the surface mailbox and then immutable until the render actor is done.
// Coordinates are physical pixels; colors are linear, premultiplied.
#pragma once

#include <stdint.h>

#include "ui/handle.h"
#include "ui/result.h"

#define UI_MAX_SURFACES 8u

typedef struct ui_dl_rect {
    float x, y, width, height;
    float radius;
    float reserved[3];
    float color[4];
} ui_dl_rect;

// One glyph quad: a rectangle of whole physical pixels showing the same-sized rectangle of
// an atlas page at (u, v), tinted by a linear premultiplied color.
typedef struct ui_dl_glyph {
    float x, y, width, height;
    float u, v;
    float reserved[2];
    float color[4];
} ui_dl_glyph;

typedef enum ui_dl_kind {
    UI_DL_RECTS = 0,
    UI_DL_GLYPHS,
} ui_dl_kind;

// Paint order: batches draw in sequence; each covers a contiguous range of rects or of
// glyphs (all on one atlas page).
typedef struct ui_dl_batch {
    uint16_t kind;  // ui_dl_kind
    uint16_t page;  // glyph batches: atlas page
    uint32_t first, count;
} ui_dl_batch;

typedef struct ui_display_list {
    ui_handle surface;
    uint64_t frame_id;
    uint64_t predicted_present_host_ns; // CLOCK_MONOTONIC
    uint64_t anim_time_ns;              // animation-clock time the list was sampled at
    uint32_t width, height;             // physical pixels
    float logical_width, logical_height;
    uint32_t scale_120;
    uint64_t platform_token; // platform state this size corresponds to (Wayland configure serial)
    float clear[4];
    uint64_t epoch;      // core frame the list was built in (glyph eviction bound)
    uint64_t atlas_seq;  // newest atlas update the list's glyphs need applied first
    float text_contrast;  // coverage contrast boost (docs/text.md)
    float text_gamma;     // coverage gamma (1: off)
    uint32_t rect_count, rect_capacity;
    ui_dl_rect *rects;
    uint32_t glyph_count, glyph_capacity;
    ui_dl_glyph *glyphs;
    uint32_t batch_count, batch_capacity;
    ui_dl_batch *batches;
} ui_display_list;

void ui_display_list_reset(ui_display_list *dl);
ui_result ui_display_list_push_rect(ui_display_list *dl, const ui_dl_rect *rect);
ui_result ui_display_list_push_glyph(ui_display_list *dl, uint16_t page, const ui_dl_glyph *glyph);
void ui_display_list_free(ui_display_list *dl);
