// Renderer-agnostic display list: what one surface shows in one frame. Built by the core,
// published through the surface mailbox and then immutable until the render actor is done.
// Coordinates are physical pixels; colors are linear, premultiplied.
#pragma once

#include <stdint.h>

#include "ui/handle.h"
#include "ui/result.h"

#define UI_MAX_SURFACES 8u

// A rounded rectangle with an optional border inside its edge, as in CSS: the background
// covers the whole rect (under the border), the border is drawn over it. A layer's
// composite quad uses the same record (see ui_dl_layer).
typedef struct ui_dl_rect {
    float x, y, width, height;
    float radius;
    float border_width;
    float reserved[2];
    float color[4];
    float border_color[4];
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
    UI_DL_LAYER_BEGIN, // `first`: the layer; what follows draws into it
    UI_DL_LAYER_END,   // `page`: the layer; `first`: its composite quad in `rects`
} ui_dl_kind;

// An opacity group: content between its begin and end batches is drawn into an offscreen
// texture, which is then composited at `opacity` (CSS group opacity). The bounds are what
// the content covers, intersected with the clip at the begin batch and with the surface.
typedef struct ui_dl_layer {
    int32_t x, y, width, height; // physical px; empty (0 wide or high): draw nothing
    float opacity;
    uint32_t parent;             // enclosing layer + 1, or 0 (used while building)
    float extent[4];             // content bounds while building: x0, y0, x1, y1
} ui_dl_layer;

// A scissor rectangle in physical pixels. UI_DL_NO_CLIP covers everything.
typedef struct ui_dl_clip {
    int32_t x, y, width, height;
} ui_dl_clip;

#define UI_DL_NO_CLIP ((ui_dl_clip){0, 0, INT32_MAX, INT32_MAX})

// Paint order: batches draw in sequence; each covers a contiguous range of rects or of
// glyphs (all on one atlas page), or begins or ends a layer.
typedef struct ui_dl_batch {
    uint16_t kind;  // ui_dl_kind
    uint16_t page;  // glyph batches: atlas page
    uint32_t first, count;
    ui_dl_clip clip;
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
    uint32_t layer_count, layer_capacity;
    ui_dl_layer *layers;
    ui_dl_clip clip;      // applies to what is pushed next (a new clip starts a new batch)
    uint32_t open_layer;  // the innermost layer being built + 1, or 0
} ui_display_list;

void ui_display_list_reset(ui_display_list *dl);
ui_result ui_display_list_push_rect(ui_display_list *dl, const ui_dl_rect *rect);
ui_result ui_display_list_push_glyph(ui_display_list *dl, uint16_t page, const ui_dl_glyph *glyph);
// Opens a layer composited at `opacity`; pushes until the matching end draw into it.
ui_result ui_display_list_begin_layer(ui_display_list *dl, float opacity);
// Closes the innermost layer: fixes its bounds and adds its composite quad, drawn with the
// clip that was current when it began.
ui_result ui_display_list_end_layer(ui_display_list *dl);
void ui_display_list_free(ui_display_list *dl);
