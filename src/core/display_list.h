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
    uint32_t rect_count, rect_capacity;
    ui_dl_rect *rects;
} ui_display_list;

void ui_display_list_reset(ui_display_list *dl);
ui_result ui_display_list_push_rect(ui_display_list *dl, const ui_dl_rect *rect);
void ui_display_list_free(ui_display_list *dl);
