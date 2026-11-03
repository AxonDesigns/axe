// The set_text op end to end on the core thread's side: arbitrary bytes as the payload of a
// set_text (and, past 4 KiB, of the append_text ops after it), applied to a scene and
// resolved (fields it leaves to the node are derived from the node's values), then the
// node's paragraph laid out with the bundled fonts at a scale and width taken from the
// input, then hit-tested.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "core/scene.h"
#include "text/hittest.h"
#include "wire/gen/wire_gen.h"

#define FONT(name) UI_FUZZ_FONTS "/" name

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static ui_logger quiet = {UI_LOG_ERROR};
static ui_font_registry *fonts;
static ui_text_context ctx;
static ui_atlas *atlas;
static ui_raster raster;

static void init_once(void) {
    if (fonts) return;
    ui_font_registry_desc d = {.log = &quiet};
    ui_font_registry_create(&d, &fonts);
    const char *files[] = {FONT("NotoSans-VF.ttf"), FONT("NotoSansHebrew-VF.ttf"),
                           FONT("NotoSansArabic-VF.ttf"), FONT("NotoSansDevanagari-VF.ttf"),
                           FONT("NotoSansSC-Subset.ttf")};
    for (size_t i = 0; i < sizeof files / sizeof *files; i++)
        ui_font_registry_add_file(fonts, files[i], 0, NULL);
    ui_text_context_init(&ctx, fonts);
    ui_atlas_create(&(ui_atlas_desc){.page_size = 256, .max_pages = 2}, &atlas);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    init_once();
    if (size < 8) return 0;
    float scale = 0.5f + (float)(data[0] % 8) * 0.25f;
    float width = (float)(data[1] | data[2] << 8) / 64.f;
    data += 3;
    size -= 3;

    ui_scene s;
    ui_scene_init(&s, &quiet);
    ui_handle surface = {(uint32_t)UI_HANDLE_SURFACE << UI_HANDLE_SLOT_BITS, 1};
    ui_handle node = {1, 1};
    ui_scene_add_root(&s, surface);
    ui_wire_buf b;
    ui_wire_buf_init(&b, UI_WIRE_MAGIC_CMD);
    uint8_t *p = ui_wire_buf_op(&b, UI_OP_CREATE_NODE, UI_WIRE_CREATE_NODE_SIZE);
    p = ui_wire_put_handle(p, node);
    ui_wire_put_handle(p, surface);
    p = ui_wire_buf_op(&b, UI_OP_SET_RECT, UI_WIRE_SET_RECT_SIZE);
    p = ui_wire_put_handle(p, node);
    for (int i = 0; i < 4; i++) p = ui_wire_put_f32(p, i == 2 ? width : 10);
    // The input becomes the op payloads, with the node handle in front so ops reach it.
    size_t first = size < 4096 ? size : 4096;
    uint32_t pay = ui_wire_pad4((uint32_t)first + 8);
    p = ui_wire_buf_op(&b, UI_OP_SET_TEXT, pay);
    memset(p, 0, pay);
    p = ui_wire_put_handle(p, node);
    memcpy(p, data, first);
    for (size_t at = first; at < size; at += 4096) {
        size_t n = size - at < 4096 ? size - at : 4096;
        pay = ui_wire_pad4((uint32_t)n + 8);
        p = ui_wire_buf_op(&b, UI_OP_APPEND_TEXT, pay);
        memset(p, 0, pay);
        p = ui_wire_put_handle(p, node);
        memcpy(p, data + at, n);
    }
    size_t len;
    uint8_t *bytes = ui_wire_buf_finish(&b, &len);
    ui_scene_apply(&s, bytes, len, 0); // malformed framing is a valid outcome
    ui_scene_resolve(&s, 0); // fields left to the node make a derived source (paragraph.h)
    free(bytes);

    ui_paragraph *para = s.handles.used > 1 && s.handles.alive[1] ? s.nodes[1].text : NULL;
    if (para && para->len < (1u << 16)) {
        ui_result res;
        const ui_paragraph_layout *l = ui_paragraph_layout_get(para, &ctx, scale, width * scale, &res);
        if (l) {
            ui_atlas_begin_frame(atlas, 1, UINT64_MAX);
            for (uint32_t r = 0; r < l->run_count && r < 16; r++) {
                const ui_layout_run *run = &l->runs[r];
                ui_font *font = ui_font_registry_font_by_key(ctx.fonts, run->font);
                for (uint32_t i = 0; font && i < run->glyph_count && i < 16; i++) {
                    uint32_t gid = l->glyphs[run->glyph_start + i].glyph;
                    ui_atlas_get(atlas, &raster, font, (ui_glyph_key){run->font, gid, i % 4});
                }
            }
            ui_atlas_end_frame(atlas);
            bool failed;
            free(ui_atlas_take_update(atlas, &failed));
            ui_text_position pos;
            ui_text_rect rects[8];
            uint32_t count;
            for (int k = 0; k < 4; k++) {
                float x = (float)(k * 37 % 200) - 20, y = (float)(k * 23 % 120) - 10;
                if (ui_text_hit_point(para, &ctx, x, y, &pos) != UI_OK) abort();
                if (pos.offset > para->len) abort();
                ui_text_caret_rect(para, &ctx, pos, 1, rects);
            }
            ui_text_selection_rects(para, &ctx, 0, para->len, rects, 8, &count);
        }
        // What layout asks: min-content no wider than max-content, and heights that don't
        // shrink as the width does.
        float min_w = 0;
        ui_text_measure wide, narrow;
        if (ui_paragraph_min_content(para, &ctx, scale, &min_w) == UI_OK &&
            ui_paragraph_measure(para, &ctx, scale, INFINITY, &wide) == UI_OK &&
            ui_paragraph_measure(para, &ctx, scale, min_w, &narrow) == UI_OK) {
            if (min_w > wide.width + 1e-3f * (1 + wide.width)) abort();
            if (para->max_lines == 0 && narrow.height + 1e-3f < wide.height) abort();
        }
    }
    ui_scene_destroy(&s);
    return 0;
}
