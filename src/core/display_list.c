#include "core/display_list.h"

#include <math.h>

#include "runtime/common.h"

void ui_display_list_reset(ui_display_list *dl) {
    dl->rect_count = 0;
    dl->glyph_count = 0;
    dl->batch_count = 0;
    dl->layer_count = 0;
    dl->clip = UI_DL_NO_CLIP;
    dl->open_layer = 0;
}

static bool same_clip(ui_dl_clip a, ui_dl_clip b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

static ui_result new_batch(ui_display_list *dl, ui_dl_batch b) {
    ui_result r = ui_grow((void **)&dl->batches, &dl->batch_capacity, dl->batch_count + 1,
                          sizeof *dl->batches);
    if (r == UI_OK) dl->batches[dl->batch_count++] = b;
    return r;
}

// Extends the last batch when it continues the same kind (and page), else starts one.
static ui_result batch(ui_display_list *dl, ui_dl_kind kind, uint16_t page, uint32_t index) {
    if (dl->batch_count) {
        ui_dl_batch *last = &dl->batches[dl->batch_count - 1];
        if (last->kind == kind && last->page == page && last->first + last->count == index &&
            same_clip(last->clip, dl->clip)) {
            last->count++;
            return UI_OK;
        }
    }
    return new_batch(dl, (ui_dl_batch){(uint16_t)kind, page, index, 1, dl->clip});
}

// Grows the open layer's content bounds.
static void extend(ui_display_list *dl, float x0, float y0, float x1, float y1) {
    if (!dl->open_layer) return;
    float *e = dl->layers[dl->open_layer - 1].extent;
    e[0] = fminf(e[0], x0);
    e[1] = fminf(e[1], y0);
    e[2] = fmaxf(e[2], x1);
    e[3] = fmaxf(e[3], y1);
}

ui_result ui_display_list_push_rect(ui_display_list *dl, const ui_dl_rect *rect) {
    ui_result r = ui_grow((void **)&dl->rects, &dl->rect_capacity, dl->rect_count + 1,
                          sizeof *dl->rects);
    if (r == UI_OK) r = batch(dl, UI_DL_RECTS, 0, dl->rect_count);
    if (r != UI_OK) return r;
    dl->rects[dl->rect_count++] = *rect;
    // One pixel of margin: the rect shader's anti-aliasing ramp.
    extend(dl, rect->x - 1, rect->y - 1, rect->x + rect->width + 1, rect->y + rect->height + 1);
    return UI_OK;
}

ui_result ui_display_list_push_glyph(ui_display_list *dl, uint16_t page, const ui_dl_glyph *glyph) {
    ui_result r = ui_grow((void **)&dl->glyphs, &dl->glyph_capacity, dl->glyph_count + 1,
                          sizeof *dl->glyphs);
    if (r == UI_OK) r = batch(dl, UI_DL_GLYPHS, page, dl->glyph_count);
    if (r != UI_OK) return r;
    dl->glyphs[dl->glyph_count++] = *glyph;
    extend(dl, glyph->x, glyph->y, glyph->x + glyph->width, glyph->y + glyph->height);
    return UI_OK;
}

ui_result ui_display_list_begin_layer(ui_display_list *dl, float opacity) {
    ui_result r = ui_grow((void **)&dl->layers, &dl->layer_capacity, dl->layer_count + 1,
                          sizeof *dl->layers);
    if (r == UI_OK) r = new_batch(dl, (ui_dl_batch){UI_DL_LAYER_BEGIN, 0, dl->layer_count, 0, dl->clip});
    if (r != UI_OK) return r;
    dl->layers[dl->layer_count] = (ui_dl_layer){
        .opacity = opacity, .parent = dl->open_layer,
        .extent = {INFINITY, INFINITY, -INFINITY, -INFINITY},
    };
    dl->open_layer = ++dl->layer_count;
    return UI_OK;
}

// A bound in [0, limit]; NaN becomes 0 (fmax returns the other operand). Extents can be
// infinite (an empty layer) or huge (nodes far away), so they're clamped before conversion.
static double clamp_px(double v, double limit) { return fmin(fmax(v, 0), limit); }

ui_result ui_display_list_end_layer(ui_display_list *dl) {
    uint32_t index = dl->open_layer - 1;
    ui_dl_layer *l = &dl->layers[index];
    dl->open_layer = l->parent;
    // The begin batch's clip is the clip of the whole group.
    ui_dl_clip clip = UI_DL_NO_CLIP;
    for (uint32_t i = dl->batch_count; i-- > 0;) {
        const ui_dl_batch *b = &dl->batches[i];
        if (b->kind == UI_DL_LAYER_BEGIN && b->first == index) {
            clip = b->clip;
            break;
        }
    }
    double x0 = clamp_px(floor(l->extent[0]), dl->width), y0 = clamp_px(floor(l->extent[1]), dl->height);
    double x1 = clamp_px(ceil(l->extent[2]), dl->width), y1 = clamp_px(ceil(l->extent[3]), dl->height);
    x0 = fmax(x0, clip.x);
    y0 = fmax(y0, clip.y);
    x1 = fmin(x1, (double)clip.x + clip.width);
    y1 = fmin(y1, (double)clip.y + clip.height);
    if (!(x1 > x0 && y1 > y0)) x1 = x0, y1 = y0; // nothing visible: an empty layer
    l->x = (int32_t)x0;
    l->y = (int32_t)y0;
    l->width = (int32_t)(x1 - x0);
    l->height = (int32_t)(y1 - y0);
    ui_dl_rect quad = {
        .x = (float)l->x, .y = (float)l->y, .width = (float)l->width, .height = (float)l->height,
        .color = {l->opacity, l->opacity, l->opacity, l->opacity},
    };
    ui_dl_clip saved = dl->clip;
    dl->clip = clip;
    ui_result r = ui_grow((void **)&dl->rects, &dl->rect_capacity, dl->rect_count + 1,
                          sizeof *dl->rects);
    if (r == UI_OK)
        r = new_batch(dl, (ui_dl_batch){UI_DL_LAYER_END, (uint16_t)index, dl->rect_count, 1, clip});
    dl->clip = saved;
    if (r != UI_OK) return r;
    dl->rects[dl->rect_count++] = quad;
    if (l->width) extend(dl, quad.x, quad.y, quad.x + quad.width, quad.y + quad.height);
    return UI_OK;
}

void ui_display_list_free(ui_display_list *dl) {
    free(dl->rects);
    free(dl->glyphs);
    free(dl->batches);
    free(dl->layers);
    dl->rects = NULL;
    dl->glyphs = NULL;
    dl->batches = NULL;
    dl->layers = NULL;
    dl->rect_count = dl->rect_capacity = 0;
    dl->glyph_count = dl->glyph_capacity = 0;
    dl->batch_count = dl->batch_capacity = 0;
    dl->layer_count = dl->layer_capacity = 0;
}
