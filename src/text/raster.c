#include "text/raster.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/common.h"

#define MAX_SUBDIVISIONS 128
#define MAX_BITMAP 4096 // px per side; larger glyphs are refused

void ui_raster_free(ui_raster *r) {
    free(r->segs);
    free(r->acc);
    free(r->pixels);
    memset(r, 0, sizeof *r);
}

void ui_raster_begin(ui_raster *r) {
    r->seg_count = 0;
    r->failed = false;
    r->cx = r->cy = r->sx = r->sy = 0;
    r->min_x = r->min_y = INFINITY;
    r->max_x = r->max_y = -INFINITY;
}

static void add_line(ui_raster *r, float x0, float y0, float x1, float y1) {
    r->min_x = fminf(r->min_x, fminf(x0, x1));
    r->max_x = fmaxf(r->max_x, fmaxf(x0, x1));
    r->min_y = fminf(r->min_y, fminf(y0, y1));
    r->max_y = fmaxf(r->max_y, fmaxf(y0, y1));
    if (y0 == y1) return; // horizontal lines add no area
    if (ui_grow((void **)&r->segs, &r->seg_capacity, r->seg_count + 1, sizeof *r->segs) != UI_OK) {
        r->failed = true;
        return;
    }
    r->segs[r->seg_count++] = (ui_raster_segment){x0, y0, x1, y1};
}

static void sink_move(void *u, float x, float y) {
    ui_raster *r = u;
    if (r->cx != r->sx || r->cy != r->sy) add_line(r, r->cx, r->cy, r->sx, r->sy);
    r->cx = r->sx = x;
    r->cy = r->sy = y;
}

static void sink_line(void *u, float x, float y) {
    ui_raster *r = u;
    add_line(r, r->cx, r->cy, x, y);
    r->cx = x;
    r->cy = y;
}

// Uniform subdivision with a segment count bounded by the curve's second derivative: a
// chord over parameter step h deviates at most |B''| h^2 / 8 from the curve.
static uint32_t segments_for(float dd) {
    float n = ceilf(sqrtf(dd / (8.f * UI_RASTER_TOLERANCE)));
    return n < 1 ? 1 : n > MAX_SUBDIVISIONS ? MAX_SUBDIVISIONS : (uint32_t)n;
}

static void sink_quad(void *u, float cx, float cy, float x, float y) {
    ui_raster *r = u;
    float x0 = r->cx, y0 = r->cy;
    float ddx = x0 - 2 * cx + x, ddy = y0 - 2 * cy + y; // B'' = 2 (p0 - 2 p1 + p2)
    uint32_t n = segments_for(2.f * sqrtf(ddx * ddx + ddy * ddy));
    float px = x0, py = y0;
    for (uint32_t i = 1; i <= n; i++) {
        float t = (float)i / (float)n, mt = 1 - t;
        float qx = mt * mt * x0 + 2 * mt * t * cx + t * t * x;
        float qy = mt * mt * y0 + 2 * mt * t * cy + t * t * y;
        add_line(r, px, py, qx, qy);
        px = qx;
        py = qy;
    }
    r->cx = x;
    r->cy = y;
}

static void sink_cubic(void *u, float c1x, float c1y, float c2x, float c2y, float x, float y) {
    ui_raster *r = u;
    float x0 = r->cx, y0 = r->cy;
    // B'' = 6 ((1-t)(p0 - 2p1 + p2) + t (p1 - 2p2 + p3)), bounded by 6 max of the two.
    float ax = x0 - 2 * c1x + c2x, ay = y0 - 2 * c1y + c2y;
    float bx = c1x - 2 * c2x + x, by = c1y - 2 * c2y + y;
    float m = fmaxf(sqrtf(ax * ax + ay * ay), sqrtf(bx * bx + by * by));
    uint32_t n = segments_for(6.f * m);
    float px = x0, py = y0;
    for (uint32_t i = 1; i <= n; i++) {
        float t = (float)i / (float)n, mt = 1 - t;
        float a = mt * mt * mt, b = 3 * mt * mt * t, c = 3 * mt * t * t, d = t * t * t;
        float qx = a * x0 + b * c1x + c * c2x + d * x;
        float qy = a * y0 + b * c1y + c * c2y + d * y;
        add_line(r, px, py, qx, qy);
        px = qx;
        py = qy;
    }
    r->cx = x;
    r->cy = y;
}

static void sink_close(void *u) {
    ui_raster *r = u;
    if (r->cx != r->sx || r->cy != r->sy) add_line(r, r->cx, r->cy, r->sx, r->sy);
    r->cx = r->sx;
    r->cy = r->sy;
}

ui_path_sink ui_raster_sink(ui_raster *r) {
    return (ui_path_sink){sink_move, sink_line, sink_quad, sink_cubic, sink_close, r};
}

// Adds the exact signed area of one segment (bitmap coordinates, y down) to acc, whose rows
// are `stride` floats. The area left of the segment in each row becomes coverage after the
// row's prefix sum. The scheme is the trapezoid accumulation of font-rs and stb_truetype 2.
static void accumulate(float *acc, uint32_t stride, uint32_t h, float x0, float y0, float x1,
                       float y1) {
    float dir = 1;
    if (y0 > y1) {
        dir = -1;
        float t = x0;
        x0 = x1;
        x1 = t;
        t = y0;
        y0 = y1;
        y1 = t;
    }
    float dxdy = (x1 - x0) / (y1 - y0);
    float x = x0;
    if (y0 < 0) {
        x -= y0 * dxdy;
        y0 = 0;
    }
    float ymax = fminf(y1, (float)h);
    for (int32_t y = (int32_t)y0; (float)y < ymax; y++) {
        float *row = acc + (size_t)y * stride;
        float dy = fminf((float)y + 1, ymax) - fmaxf((float)y, y0);
        float xnext = x + dxdy * dy;
        float d = dy * dir;
        float xa = fminf(x, xnext), xb = fmaxf(x, xnext);
        float xa_floor = floorf(xa);
        int32_t ia = (int32_t)xa_floor;
        int32_t ib = (int32_t)ceilf(xb);
        if (ib <= ia + 1) {
            // Within one column: the area right of the segment's midpoint goes to the next.
            float xm = 0.5f * (x + xnext) - xa_floor;
            row[ia] += d - d * xm;
            row[ia + 1] += d * xm;
        } else {
            float s = 1.f / (xb - xa);
            float fa = xa - xa_floor;
            float a0 = 0.5f * s * (1 - fa) * (1 - fa);
            float fb = xb - (float)ib + 1;
            float am = 0.5f * s * fb * fb;
            row[ia] += d * a0;
            if (ib == ia + 2) {
                row[ia + 1] += d * (1 - a0 - am);
            } else {
                float a1 = s * (1.5f - fa);
                row[ia + 1] += d * (a1 - a0);
                for (int32_t xi = ia + 2; xi < ib - 1; xi++) row[xi] += d * s;
                float a2 = a1 + (float)(ib - ia - 3) * s;
                row[ib - 1] += d * (1 - a2 - am);
            }
            row[ib] += d * am;
        }
        x = xnext;
    }
}

ui_result ui_raster_end(ui_raster *r, float dx, float dy, ui_bitmap *out) {
    sink_close(r);
    if (r->failed) return UI_ERR_OUT_OF_MEMORY;
    *out = (ui_bitmap){0};
    if (!r->seg_count) return UI_OK;
    // Pixel bounds in y-down bitmap space: x' = x + dx - left, y' = top - (y + dy).
    int32_t left = (int32_t)floorf(r->min_x + dx);
    int32_t right = (int32_t)ceilf(r->max_x + dx);
    int32_t top = (int32_t)ceilf(r->max_y + dy);
    int32_t bottom = (int32_t)floorf(r->min_y + dy);
    if (right <= left || top <= bottom) return UI_OK;
    if (right - left > MAX_BITMAP || top - bottom > MAX_BITMAP) return UI_ERR_INVALID_ARGUMENT;
    uint32_t w = (uint32_t)(right - left), h = (uint32_t)(top - bottom);
    uint32_t stride = w + 2; // ib can reach w, and the midpoint rule writes ia + 1
    uint32_t cells = stride * h;
    if (ui_grow((void **)&r->acc, &r->acc_capacity, cells, sizeof *r->acc) != UI_OK ||
        ui_grow((void **)&r->pixels, &r->pixel_capacity, w * h, 1) != UI_OK)
        return UI_ERR_OUT_OF_MEMORY;
    memset(r->acc, 0, cells * sizeof *r->acc);
    float ox = dx - (float)left, oy = (float)top - dy;
    for (uint32_t i = 0; i < r->seg_count; i++) {
        const ui_raster_segment *s = &r->segs[i];
        accumulate(r->acc, stride, h, s->x0 + ox, oy - s->y0, s->x1 + ox, oy - s->y1);
    }
    for (uint32_t y = 0; y < h; y++) {
        const float *row = r->acc + (size_t)y * stride;
        uint8_t *dst = r->pixels + (size_t)y * w;
        float sum = 0;
        for (uint32_t x = 0; x < w; x++) {
            sum += row[x];
            float c = fminf(fabsf(sum), 1.f);
            dst[x] = (uint8_t)(c * 255.f + 0.5f);
        }
    }
    *out = (ui_bitmap){left, -top, w, h, r->pixels};
    return UI_OK;
}

ui_result ui_raster_glyph(ui_raster *r, ui_font *font, uint32_t glyph, float dx, ui_bitmap *out) {
    ui_raster_begin(r);
    ui_path_sink sink = ui_raster_sink(r);
    if (!ui_font_draw_glyph(font, glyph, &sink)) {
        *out = (ui_bitmap){0};
        return UI_OK;
    }
    return ui_raster_end(r, dx, 0, out);
}
