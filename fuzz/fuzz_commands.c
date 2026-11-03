// Whole command buffers on the core thread's side: arbitrary bytes as the ops of a buffer
// (the harness writes the 16-byte header so inputs don't have to guess sizes), validated
// and applied to a scene, then animated and drawn into a display list. Covers the framing
// rules of every op, including payloads from older and newer writers, handle checks, the
// clip stack and culling. Text is laid out by fuzz_set_text; here it is not drawn.
#include <stdlib.h>
#include <string.h>

#include "core/scene.h"
#include "wire/wire.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static ui_logger quiet = {UI_LOG_ERROR};

static void ignore_timer(void *user, uint32_t id, uint64_t deadline_ns) {
    (void)user, (void)id, (void)deadline_ns;
}

static void ignore_font(void *user, uint32_t id, const char *path, uint32_t len) {
    (void)user, (void)id, (void)path, (void)len;
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

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 4 || size > (1u << 20)) return 0;
    uint32_t scale_120 = 60 + (uint32_t)(data[0] % 8) * 30;
    data += 1;
    size -= 1;
    size_t body = size & ~(size_t)3;
    uint8_t *buf = malloc(UI_WIRE_HEADER_SIZE + body + 1);
    if (!buf) return 0;
    uint8_t *p = ui_wire_put_u32(buf, UI_WIRE_MAGIC_CMD);
    p = ui_wire_put_u16(p, UI_WIRE_VERSION);
    p = ui_wire_put_u16(p, 0);
    p = ui_wire_put_u32(p, (uint32_t)(UI_WIRE_HEADER_SIZE + body));
    ui_wire_put_u32(p, count_ops(data, body));
    memcpy(buf + UI_WIRE_HEADER_SIZE, data, body);

    ui_scene s;
    ui_scene_init(&s, &quiet);
    s.timer_fn = ignore_timer;
    s.font_fn = ignore_font;
    // Two surfaces, so ops can name either or a stale one.
    ui_handle surfaces[2] = {{(uint32_t)UI_HANDLE_SURFACE << UI_HANDLE_SLOT_BITS, 1},
                             {(uint32_t)UI_HANDLE_SURFACE << UI_HANDLE_SLOT_BITS | 1, 1}};
    ui_scene_add_root(&s, surfaces[0]);
    ui_scene_add_root(&s, surfaces[1]);
    // Applying twice runs the ops again against the state the first pass left (nodes that
    // now exist, slots whose generation moved on).
    for (int pass = 0; pass < 2; pass++) {
        ui_scene_apply(&s, buf, UI_WIRE_HEADER_SIZE + body, (uint64_t)pass * 16000000u);
        ui_scene_animate(&s, (uint64_t)(pass + 1) * 16000000u);
        for (int i = 0; i < 2; i++) {
            ui_display_list dl = {.surface = surfaces[i], .width = 400, .height = 300,
                                  .logical_width = 400.f * 120 / (float)scale_120,
                                  .logical_height = 300.f * 120 / (float)scale_120,
                                  .scale_120 = scale_120};
            ui_display_list_reset(&dl);
            ui_scene_build(&s, surfaces[i], &dl, NULL);
            ui_display_list_free(&dl);
        }
    }
    ui_scene_destroy(&s);
    free(buf);
    return 0;
}
