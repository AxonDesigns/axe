// Coverage rasterizer: paths in, 8-bit coverage out. Curves are flattened into line
// segments (adaptive to a 1/16 px tolerance) and every segment adds its exact signed area to
// an accumulation buffer; a prefix sum per row then gives each pixel's exact coverage of the
// flattened outline. No supersampling. Coverage is min(1, |winding area|): exact for
// non-overlapping contours and for overlapping contours of the same direction (variable
// fonts overlap contours this way); opposite-direction overlaps cancel as even-odd would.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "text/hb/hb_font.h"
#include "ui/result.h"

#define UI_RASTER_TOLERANCE (1.f / 16.f)

typedef struct ui_raster_segment {
    float x0, y0, x1, y1;
} ui_raster_segment;

// Reusable state; zero-initialize, free with ui_raster_free. Core thread.
typedef struct ui_raster {
    ui_raster_segment *segs;
    uint32_t seg_count, seg_capacity;
    float *acc;
    uint32_t acc_capacity;
    uint8_t *pixels;
    uint32_t pixel_capacity;
    float cx, cy, sx, sy; // current point, subpath start
    float min_x, min_y, max_x, max_y;
    bool failed;          // an allocation failed while collecting segments
} ui_raster;

void ui_raster_free(ui_raster *r);

typedef struct ui_bitmap {
    int32_t left;       // pixel column of the bitmap's left edge, relative to the origin
    int32_t top;        // pixel row of its top edge, y down: -top is above the baseline
    uint32_t width, height;
    const uint8_t *pixels; // width * height coverage values, rows top to bottom; owned by r
} ui_bitmap;

// Path interface (coordinates in px, y up), for glyphs and tests.
void ui_raster_begin(ui_raster *r);
ui_path_sink ui_raster_sink(ui_raster *r);
// Rasterizes the collected path offset by (dx, dy) px. An empty path gives a 0x0 bitmap.
// Fails only on allocation.
ui_result ui_raster_end(ui_raster *r, float dx, float dy, ui_bitmap *out);

// Rasterizes a glyph at a horizontal subpixel offset `dx` (0 <= dx < 1).
ui_result ui_raster_glyph(ui_raster *r, ui_font *font, uint32_t glyph, float dx, ui_bitmap *out);
