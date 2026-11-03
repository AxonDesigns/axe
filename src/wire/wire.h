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

// Payload sizes (bytes after the 4-byte op header). Each is the op's original size, the
// smallest payload a reader accepts: newer writers may append fields, which older readers
// ignore (docs/command-buffer.md, "Evolving the format").
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
    UI_WIRE_SIZE_SET_CLIP = 12,          // node, flags u32 (bit 0: clip)
    UI_WIRE_SIZE_LOAD_FONT_HEADER = 8,   // id u32, path_bytes u16, header_bytes u16; path
    UI_WIRE_SIZE_FONT_LOADED = 8,        // id u32, result u32
    UI_WIRE_SIZE_DEFINE_STYLE_HEADER = 8, // id u32, header_bytes u16, block_count u16; blocks
    UI_WIRE_SIZE_STYLE_BLOCK_HEADER = 8, // block_bytes u16, states u8, 0, entry_count u16, 0
    UI_WIRE_SIZE_DESTROY_STYLE = 4,      // id u32
    UI_WIRE_SIZE_SET_STYLES_HEADER = 12, // node, count u16, header_bytes u16; ids
    UI_WIRE_SIZE_SET_PROPS_HEADER = 12,  // node, header_bytes u16, entry_count u16; entries
    UI_WIRE_SIZE_SET_TOKEN_HEADER = 4,   // id u32; one entry (property 0)
    UI_WIRE_SIZE_SET_STATE = 16,         // node, mask u32, value u32
    UI_WIRE_SIZE_MOVE_NODE = 24,         // node, parent, before (null: append)
    UI_WIRE_SIZE_REQUEST_LAYOUT = 12,    // node, request_id u32
    UI_WIRE_SIZE_LAYOUT_MEASURED = 88,   // request_id u32, result u32, node, surface, 4 rects
};

// A style entry: u16 property, u8 kind, u8 words (its size in 4-byte words, header
// included), then the value. Readers skip entries they don't understand by `words`.
#define UI_WIRE_STYLE_ENTRY_HEADER 4u

// set_text: fixed header, then spans, the family-name region (NUL-terminated names,
// zero-padded to a multiple of 4), the ellipsis (padded), the first text chunk (padded).
// The header grew from 64 bytes (line_height, align); readers find the spans at the
// header size the op carries, and each span at the span size it carries.
#define UI_WIRE_SIZE_SET_TEXT_HEADER 72u    // what this writer sends
#define UI_WIRE_MIN_SET_TEXT_HEADER 64u     // the original header
#define UI_WIRE_SIZE_TEXT_SPAN 36u
#define UI_WIRE_SIZE_APPEND_TEXT_HEADER 12u // node, chunk_bytes u16, header_bytes u16; chunk
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

// Header and record sizes that variable-length ops carry. Writers from before a size field
// existed left its bytes 0, which stands for the original size.
static inline uint32_t ui_wire_size_or(const uint8_t *p, uint32_t original) {
    uint32_t n = ui_wire_get_u16(p);
    return n ? n : original;
}
static inline uint32_t ui_wire_set_text_header(const uint8_t *payload) {
    return ui_wire_size_or(payload + 14, UI_WIRE_MIN_SET_TEXT_HEADER);
}
static inline uint32_t ui_wire_set_text_span(const uint8_t *payload) {
    return ui_wire_size_or(payload + 28, UI_WIRE_SIZE_TEXT_SPAN);
}
static inline uint32_t ui_wire_append_text_header(const uint8_t *payload) {
    return ui_wire_size_or(payload + 10, UI_WIRE_SIZE_APPEND_TEXT_HEADER);
}
static inline uint32_t ui_wire_load_font_header(const uint8_t *payload) {
    return ui_wire_size_or(payload + 6, UI_WIRE_SIZE_LOAD_FONT_HEADER);
}
static inline uint32_t ui_wire_define_style_header(const uint8_t *payload) {
    return ui_wire_size_or(payload + 4, UI_WIRE_SIZE_DEFINE_STYLE_HEADER);
}
static inline uint32_t ui_wire_set_styles_header(const uint8_t *payload) {
    return ui_wire_size_or(payload + 10, UI_WIRE_SIZE_SET_STYLES_HEADER);
}
static inline uint32_t ui_wire_set_props_header(const uint8_t *payload) {
    return ui_wire_size_or(payload + 8, UI_WIRE_SIZE_SET_PROPS_HEADER);
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

#ifdef UI_WIRE_PROOF
// The migration proof's recorder (tests/wire/proof.md): UI_WIRE_RECORD=<dir> keeps a copy of
// every buffer.
void ui_wire_proof_record(const char *kind, const uint8_t *bytes, size_t size);
#endif

// set_timer's deadline meaning "disarm".
#define UI_WIRE_TIMER_CANCEL UINT64_MAX
