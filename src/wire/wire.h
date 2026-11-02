// Shared framing for the command and event buffers (docs/command-buffer.md).
// All values are little-endian; the build asserts a little-endian target.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ui/wire.h"

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "wire format assumes little-endian");

#define UI_WIRE_MAGIC_CMD 0x42434955u   // "UICB"
#define UI_WIRE_MAGIC_EVENT 0x42454955u // "UIEB"
#define UI_WIRE_HEADER_SIZE 16u
#define UI_WIRE_OP_HEADER_SIZE 4u
#define UI_WIRE_MAX_OP_SIZE 0xFFFCu
#define UI_WIRE_MAX_BUFFER_SIZE (64u << 20)

// Payload sizes (bytes after the 4-byte op header).
#define UI_WIRE_HANDLE_SIZE 8u
enum {
    UI_WIRE_SIZE_CREATE_NODE = 16,       // node, parent
    UI_WIRE_SIZE_DESTROY_NODE = 8,       // node
    UI_WIRE_SIZE_SET_RECT = 24,          // node, x, y, w, h
    UI_WIRE_SIZE_SET_CORNER_RADIUS = 12, // node, radius
    UI_WIRE_SIZE_SET_COLOR = 24,         // node, r, g, b, a
    UI_WIRE_SIZE_START_SPRING_BASE = 20, // node, property u16, count u16, stiffness, damping
    UI_WIRE_SIZE_FRAME_COMMITTED = 24,   // surface, frame_id u64, predicted_present_ns u64
    UI_WIRE_SIZE_SURFACE_RESIZED = 20,   // surface, width, height, scale_120 u32
    UI_WIRE_SIZE_SURFACE_CLOSE = 8,      // surface
    UI_WIRE_SIZE_SET_TIMER = 16,         // id u32, reserved u32, deadline_ns u64
    UI_WIRE_SIZE_TIMER_FIRED = 24,       // id u32, reserved u32, deadline_ns u64, now_ns u64
};

// set_text: fixed header, then spans, the family-name region (NUL-terminated names,
// zero-padded to a multiple of 4), the ellipsis (padded), the first text chunk (padded).
#define UI_WIRE_SIZE_SET_TEXT_HEADER 64u
#define UI_WIRE_SIZE_TEXT_SPAN 36u
#define UI_WIRE_SIZE_APPEND_TEXT_HEADER 12u // node, chunk_bytes; then the chunk (padded)
#define UI_WIRE_TEXT_CUSTOM_ELLIPSIS 1u     // set_text flags: the ellipsis bytes replace U+2026
#define UI_WIRE_MAX_PAYLOAD (UI_WIRE_MAX_OP_SIZE - UI_WIRE_OP_HEADER_SIZE)

static inline uint32_t ui_wire_pad4(uint32_t n) { return (n + 3u) & ~3u; }

// Growable encoder. The header is written by ui_wire_buf_finish.
typedef struct ui_wire_buf {
    uint8_t *data;
    uint32_t size, capacity;
    uint32_t op_count;
    uint32_t magic;
} ui_wire_buf;

void ui_wire_buf_init(ui_wire_buf *b, uint32_t magic);
void ui_wire_buf_free(ui_wire_buf *b);
// Reserves an op with `payload` bytes (multiple of 4) and returns the payload pointer, or
// NULL on allocation failure (the buffer is unchanged).
uint8_t *ui_wire_buf_op(ui_wire_buf *b, uint16_t opcode, uint32_t payload);
// Writes the header and hands the bytes to the caller; the buffer becomes empty.
// Returns NULL with *size 0 if nothing was written.
uint8_t *ui_wire_buf_finish(ui_wire_buf *b, size_t *size);

static inline uint8_t *ui_wire_put_u32(uint8_t *p, uint32_t v) {
    __builtin_memcpy(p, &v, 4);
    return p + 4;
}
static inline uint8_t *ui_wire_put_u16(uint8_t *p, uint16_t v) {
    __builtin_memcpy(p, &v, 2);
    return p + 2;
}
static inline uint8_t *ui_wire_put_u64(uint8_t *p, uint64_t v) {
    __builtin_memcpy(p, &v, 8);
    return p + 8;
}
static inline uint8_t *ui_wire_put_f32(uint8_t *p, float v) {
    __builtin_memcpy(p, &v, 4);
    return p + 4;
}
static inline uint8_t *ui_wire_put_handle(uint8_t *p, ui_handle h) {
    p = ui_wire_put_u32(p, h.index);
    return ui_wire_put_u32(p, h.generation);
}
static inline uint32_t ui_wire_get_u32(const uint8_t *p) {
    uint32_t v;
    __builtin_memcpy(&v, p, 4);
    return v;
}
static inline uint16_t ui_wire_get_u16(const uint8_t *p) {
    uint16_t v;
    __builtin_memcpy(&v, p, 2);
    return v;
}
static inline uint64_t ui_wire_get_u64(const uint8_t *p) {
    uint64_t v;
    __builtin_memcpy(&v, p, 8);
    return v;
}
static inline float ui_wire_get_f32(const uint8_t *p) {
    float v;
    __builtin_memcpy(&v, p, 4);
    return v;
}
static inline ui_handle ui_wire_get_handle(const uint8_t *p) {
    return (ui_handle){ui_wire_get_u32(p), ui_wire_get_u32(p + 4)};
}

// Validating op iterator over a buffer with the given magic.
typedef struct ui_wire_reader {
    const uint8_t *data;
    size_t size, offset;
    uint32_t op_count, ops_read;
} ui_wire_reader;

typedef struct ui_wire_op {
    uint16_t opcode;
    uint16_t payload_size;
    const uint8_t *payload;
} ui_wire_op;

// Checks magic, version, total size and alignment.
ui_result ui_wire_reader_init(ui_wire_reader *r, uint32_t magic, const void *data, size_t size);
// UI_OK with the next op, UI_ERR_END after the last, UI_ERR_MALFORMED on a framing error
// (bad op size, overrun, op count mismatch).
ui_result ui_wire_reader_next(ui_wire_reader *r, ui_wire_op *op);

// Event writer used by the core.
ui_result ui_wire_event_frame_committed(ui_wire_buf *b, ui_handle surface, uint64_t frame_id,
                                        uint64_t predicted_present_ns);
ui_result ui_wire_event_surface_resized(ui_wire_buf *b, ui_handle surface, float width,
                                        float height, uint32_t scale_120);
ui_result ui_wire_event_surface_close(ui_wire_buf *b, ui_handle surface);
ui_result ui_wire_event_timer_fired(ui_wire_buf *b, uint32_t id, uint64_t deadline_ns,
                                    uint64_t now_ns);

// set_timer's deadline meaning "disarm".
#define UI_WIRE_TIMER_CANCEL UINT64_MAX
