#include "core/display_list.h"

#include "runtime/common.h"

void ui_display_list_reset(ui_display_list *dl) {
    dl->rect_count = 0;
    dl->glyph_count = 0;
    dl->batch_count = 0;
    dl->clip = UI_DL_NO_CLIP;
}

static bool same_clip(ui_dl_clip a, ui_dl_clip b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
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
    ui_result r = ui_grow((void **)&dl->batches, &dl->batch_capacity, dl->batch_count + 1,
                          sizeof *dl->batches);
    if (r != UI_OK) return r;
    dl->batches[dl->batch_count++] = (ui_dl_batch){(uint16_t)kind, page, index, 1, dl->clip};
    return UI_OK;
}

ui_result ui_display_list_push_rect(ui_display_list *dl, const ui_dl_rect *rect) {
    ui_result r = ui_grow((void **)&dl->rects, &dl->rect_capacity, dl->rect_count + 1,
                          sizeof *dl->rects);
    if (r == UI_OK) r = batch(dl, UI_DL_RECTS, 0, dl->rect_count);
    if (r != UI_OK) return r;
    dl->rects[dl->rect_count++] = *rect;
    return UI_OK;
}

ui_result ui_display_list_push_glyph(ui_display_list *dl, uint16_t page, const ui_dl_glyph *glyph) {
    ui_result r = ui_grow((void **)&dl->glyphs, &dl->glyph_capacity, dl->glyph_count + 1,
                          sizeof *dl->glyphs);
    if (r == UI_OK) r = batch(dl, UI_DL_GLYPHS, page, dl->glyph_count);
    if (r != UI_OK) return r;
    dl->glyphs[dl->glyph_count++] = *glyph;
    return UI_OK;
}

void ui_display_list_free(ui_display_list *dl) {
    free(dl->rects);
    free(dl->glyphs);
    free(dl->batches);
    dl->rects = NULL;
    dl->glyphs = NULL;
    dl->batches = NULL;
    dl->rect_count = dl->rect_capacity = 0;
    dl->glyph_count = dl->glyph_capacity = 0;
    dl->batch_count = dl->batch_capacity = 0;
}
