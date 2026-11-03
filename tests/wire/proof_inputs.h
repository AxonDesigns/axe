// How the fuzz harnesses turn an input into buffers (fuzz/fuzz_commands.c and
// fuzz/fuzz_set_text.c), so the proof and its fuzzer read seeds exactly as the fuzzers do.
#pragma once

#include <stdlib.h>
#include <string.h>

#include "wire/gen/wire_gen.h"

static inline uint32_t proof_count_ops(const uint8_t *body, size_t size) {
    uint32_t n = 0;
    for (size_t at = 0; at + UI_WIRE_OP_HEADER_SIZE <= size; n++) {
        uint16_t op_size = ui_wire_get_u16(body + at + 2);
        if (op_size < UI_WIRE_OP_HEADER_SIZE || op_size % 4 || op_size > size - at) return n + 1;
        at += op_size;
    }
    return n;
}

static inline uint8_t *proof_make_buffer(uint32_t magic, const uint8_t *ops, size_t size, size_t *out_size) {
    uint8_t *buf = malloc(UI_WIRE_HEADER_SIZE + size + 1);
    if (!buf) abort();
    uint8_t *p = ui_wire_put_u32(buf, magic);
    p = ui_wire_put_u16(p, UI_WIRE_VERSION);
    p = ui_wire_put_u16(p, 0);
    p = ui_wire_put_u32(p, (uint32_t)(UI_WIRE_HEADER_SIZE + size));
    ui_wire_put_u32(p, proof_count_ops(ops, size));
    memcpy(buf + UI_WIRE_HEADER_SIZE, ops, size);
    *out_size = UI_WIRE_HEADER_SIZE + size;
    return buf;
}

// fuzz_set_text: a node, a frame, and the input as a set_text payload plus continuations.
static inline uint8_t *proof_set_text_buffer(const uint8_t *data, size_t size, size_t *out_size) {
    if (size < 8) return NULL;
    data += 3;
    size -= 3;
    ui_wire_buf b;
    ui_wire_buf_init(&b, UI_WIRE_MAGIC_CMD);
    ui_handle surface = {(uint32_t)UI_HANDLE_SURFACE << UI_HANDLE_SLOT_BITS, 1}, node = {1, 1};
    ui_wire_create_node_put(&b, &(ui_wire_create_node){node, surface});
    ui_wire_set_rect_put(&b, &(ui_wire_set_rect){node, 10, 10, 10, 10});
    size_t first = size < 4096 ? size : 4096;
    uint32_t pay = ui_wire_pad4((uint32_t)first + 8);
    uint8_t *p = ui_wire_buf_op(&b, UI_OP_SET_TEXT, pay);
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
    return ui_wire_buf_finish(&b, out_size);
}
