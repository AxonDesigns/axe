#include "core/display_list.h"

#include "runtime/common.h"

void ui_display_list_reset(ui_display_list *dl) { dl->rect_count = 0; }

ui_result ui_display_list_push_rect(ui_display_list *dl, const ui_dl_rect *rect) {
    ui_result r = ui_grow((void **)&dl->rects, &dl->rect_capacity, dl->rect_count + 1,
                          sizeof *dl->rects);
    if (r != UI_OK) return r;
    dl->rects[dl->rect_count++] = *rect;
    return UI_OK;
}

void ui_display_list_free(ui_display_list *dl) {
    free(dl->rects);
    dl->rects = NULL;
    dl->rect_count = dl->rect_capacity = 0;
}
