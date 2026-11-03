// Whole command buffers on the core thread's side: arbitrary bytes as the ops of a buffer
// (the harness writes the 16-byte header so inputs don't have to guess sizes), validated
// and applied to a scene, then animated, laid out (answering layout requests) and drawn
// into a display list. Covers the framing rules of every op, including payloads from older
// and newer writers, handle checks, the clip stack and culling. Traps when preparation's
// preview mispredicts text values, or when resolving only what changed differs from
// resolving everything. Text is laid out by fuzz_set_text; here it is not drawn.
#include <stdlib.h>
#include <string.h>

#include "core/preview.h"
#include "core/scene.h"
#include "wire/wire.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static ui_logger quiet = {UI_LOG_ERROR};

static void ignore_timer(void *user, uint32_t id, uint64_t deadline_ns) {
    (void)user, (void)id, (void)deadline_ns;
}

static void ignore_font(void *user, uint32_t id, const char *path, uint32_t len,
                        uint32_t offset) {
    (void)user, (void)id, (void)path, (void)len, (void)offset;
}

// The number of ops a walk of the framing finds, plus one if it stops at a framing error
// (so the reader sees a count that matches whenever the framing is well-formed).
static uint32_t count_ops(const uint8_t *body, size_t size) {
    uint32_t n = 0;
    for (size_t at = 0; at + UI_WIRE_OP_HEADER_SIZE <= size; n++) {
        uint16_t op_size = ui_wire_get_u16(body + at + 2);
        if (op_size < UI_WIRE_OP_HEADER_SIZE || op_size % 4 || op_size > size - at) return n + 1;
        at += op_size;
    }
    return n;
}

// A buffer of the given ops (a whole number of them, or the rest after a framing error).
static uint8_t *make_buffer(const uint8_t *ops, size_t size, size_t *out_size) {
    uint8_t *buf = malloc(UI_WIRE_HEADER_SIZE + size + 1);
    if (!buf) return NULL;
    uint8_t *p = ui_wire_put_u32(buf, UI_WIRE_MAGIC_CMD);
    p = ui_wire_put_u16(p, UI_WIRE_VERSION);
    p = ui_wire_put_u16(p, 0);
    p = ui_wire_put_u32(p, (uint32_t)(UI_WIRE_HEADER_SIZE + size));
    ui_wire_put_u32(p, count_ops(ops, size));
    memcpy(buf + UI_WIRE_HEADER_SIZE, ops, size);
    *out_size = UI_WIRE_HEADER_SIZE + size;
    return buf;
}

// Applies a buffer at frame `pass`, resolves, checks, animates, lays out and draws.
static void run_pass(ui_scene *s, const uint8_t *buf, size_t size, int pass, uint32_t scale_120,
                     const ui_handle surfaces[2]) {
    // What preparation predicts the buffer makes of every node's text values must be
    // what applying it does (core/preview.h): a wrong prediction shapes on the core.
    bool valid = ui_scene_validate(buf, size) == UI_OK;
    ui_preview *pv = valid ? ui_preview_create(s, &(ui_preview_buffer){buf, size}, 1) : NULL;
    ui_scene_apply(s, buf, size, (uint64_t)pass * 16000000u);
    ui_scene_resolve(s, (uint64_t)pass * 16000000u);
    // Resolving only what changed leaves what resolving everything would.
    if (!ui_scene_check_resolved(s)) __builtin_trap();
    for (uint32_t slot = 0; pv && slot < s->handles.used; slot++) {
        if (!s->handles.alive[slot]) continue;
        ui_text_ctx want = ui_scene_text_ctx(s, slot), got;
        if (!ui_preview_ctx(pv, ui_scene_handle_of(s, slot), &got, NULL) ||
            !ui_text_values_eq(&want.values, &got.values) || want.declared != got.declared)
            __builtin_trap();
    }
    ui_preview_destroy(pv);
    ui_scene_animate(s, (uint64_t)(pass + 1) * 16000000u);
    for (int i = 0; i < 2; i++) {
        ui_display_list dl = {.surface = surfaces[i], .width = 400, .height = 300,
                              .logical_width = 400.f * 120 / (float)scale_120,
                              .logical_height = 300.f * 120 / (float)scale_120,
                              .scale_120 = scale_120};
        ui_display_list_reset(&dl);
        ui_scene_layout(s, surfaces[i], dl.logical_width, dl.logical_height, (float)scale_120 / 120.f, NULL,
                        (uint64_t)(pass + 1) * 16000000u);
        ui_scene_build(s, surfaces[i], &dl, NULL);
        ui_display_list_free(&dl);
    }
    // Layout requests, answered as the core does once laid out.
    for (uint32_t i = 0; i < s->request_count; i++) {
        float rects[4][4];
        ui_handle surface;
        ui_scene_measure(s, s->requests[i].node, rects, &surface);
    }
    s->request_count = 0;
}

// The first byte picks the surfaces' scale (bits 0-2) and a split (bits 3-7): with a split,
// the ops before it apply as one buffer and the rest as the next, so changes reach a scene
// resolved before (as tokens changing under styled nodes); then, with or without a split,
// the whole buffer applies twice.
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 4 || size > (1u << 20)) return 0;
    uint32_t scale_120 = 60 + (uint32_t)(data[0] % 8) * 30;
    uint32_t split = data[0] >> 3; // in 32nds of the ops
    data += 1;
    size -= 1;
    size_t body = size & ~(size_t)3;

    ui_scene s;
    ui_scene_init(&s, &quiet);
    s.timer_fn = ignore_timer;
    s.font_fn = ignore_font;
    // Two surfaces, so ops can name either or a stale one.
    ui_handle surfaces[2] = {{(uint32_t)UI_HANDLE_SURFACE << UI_HANDLE_SLOT_BITS, 1},
                             {(uint32_t)UI_HANDLE_SURFACE << UI_HANDLE_SLOT_BITS | 1, 1}};
    ui_scene_add_root(&s, surfaces[0]);
    ui_scene_add_root(&s, surfaces[1]);
    int pass = 0;
    if (split) {
        // The byte offset of the op at the split (framing errors end the walk).
        uint32_t ops = count_ops(data, body), want = (uint32_t)((uint64_t)ops * split / 32), n = 0;
        size_t at = 0;
        while (n < want && at + UI_WIRE_OP_HEADER_SIZE <= body) {
            uint16_t op_size = ui_wire_get_u16(data + at + 2);
            if (op_size < UI_WIRE_OP_HEADER_SIZE || op_size % 4 || op_size > body - at) break;
            at += op_size;
            n++;
        }
        size_t part_size;
        uint8_t *part = make_buffer(data, at, &part_size);
        if (part) run_pass(&s, part, part_size, pass++, scale_120, surfaces);
        free(part);
        part = make_buffer(data + at, body - at, &part_size);
        if (part) run_pass(&s, part, part_size, pass++, scale_120, surfaces);
        free(part);
    }
    // Applying twice runs the ops again against the state the first pass left (nodes that
    // now exist, slots whose generation moved on).
    size_t buf_size;
    uint8_t *buf = make_buffer(data, body, &buf_size);
    for (int i = 0; buf && i < 2; i++) run_pass(&s, buf, buf_size, pass++, scale_120, surfaces);
    ui_scene_destroy(&s);
    free(buf);
    return 0;
}
