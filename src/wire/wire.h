// Shared framing for the command and event buffers (docs/command-buffer.md): the buffer
// and op headers and the scalar accessors the generated codec (wire/gen/wire_gen.h) uses.
// All values are little-endian; the build asserts a little-endian target.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ui/wire.h"

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "wire format assumes little-endian");

#define UI_WIRE_HEADER_SIZE 16u
#define UI_WIRE_OP_HEADER_SIZE 4u
#define UI_WIRE_MAX_OP_SIZE 0xFFFCu
#define UI_WIRE_MAX_BUFFER_SIZE (64u << 20)
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

// Copies the first `sent` bytes of a fixed region into `out` (`known` bytes, what this
// reader understands): fields an older writer did not send read as 0, which every
// appended field's default must be encoded as, and fields a newer writer added are skipped.
static inline void ui_wire_read_fields(void *out, size_t known, const uint8_t *p, size_t sent) {
    size_t n = sent < known ? sent : known;
    __builtin_memcpy(out, p, n);
    __builtin_memset((uint8_t *)out + n, 0, known - n);
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

// set_timer's deadline meaning "disarm".
#define UI_WIRE_TIMER_CANCEL UINT64_MAX
