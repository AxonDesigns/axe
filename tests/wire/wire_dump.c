// Canonical dumps of command and event buffers made with the generated codec: the framing
// verdict, every field of every op, the variable data located, the style entries decoded,
// and whether the core would drop the op. tests/wire/fixtures holds the dumps the pre-IDL
// code made of the same buffers (tests/wire/proof.md); test_wire_fixtures compares them.
#include "wire_dump.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wire/gen/wire_gen.h"

// ---- Output ------------------------------------------------------------------------------

[[gnu::format(printf, 2, 3)]] static void out(wire_text *t, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char line[512];
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (t->size + (size_t)n + 1 > t->capacity) {
        size_t cap = t->capacity ? t->capacity * 2 : 4096;
        while (cap < t->size + (size_t)n + 1) cap *= 2;
        char *grown = realloc(t->data, cap);
        if (!grown) abort();
        t->data = grown;
        t->capacity = cap;
    }
    memcpy(t->data + t->size, line, (size_t)n + 1);
    t->size += (size_t)n;
}

void wire_text_free(wire_text *t) {
    free(t->data);
    *t = (wire_text){0};
}

static uint32_t bits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static uint64_t hash(const void *p, size_t n) {
    const uint8_t *b = p;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

static void handle(wire_text *t, const char *name, ui_handle h) { out(t, " %s=%08x:%u", name, h.index, h.generation); }
static void u(wire_text *t, const char *name, uint64_t v) { out(t, " %s=%llu", name, (unsigned long long)v); }
static void f(wire_text *t, const char *name, float v) { out(t, " %s=%08x", name, bits(v)); }
static void region(wire_text *t, const char *name, const uint8_t *base, const uint8_t *p, size_t n) {
    out(t, " %s@%ld:%zu#%016llx", name, (long)(p - base), n, (unsigned long long)hash(p, n));
}
static void verdict(wire_text *t, bool ok) { out(t, " -> %s", ok ? "ok" : "drop"); }

static void value(wire_text *t, const ui_value *v) {
    switch (v->kind) {
    case UI_VALUE_PX:
    case UI_VALUE_PERCENT:
    case UI_VALUE_NUMBER: f(t, "n", v->number); break;
    case UI_VALUE_COLOR:
        for (int i = 0; i < 4; i++) f(t, "c", v->color[i]);
        break;
    case UI_VALUE_KEYWORD: u(t, "k", v->keyword); break;
    case UI_VALUE_TOKEN: u(t, "t", v->token); break;
    case UI_VALUE_SPRING:
        f(t, "s", v->spring.stiffness);
        f(t, "d", v->spring.damping);
        break;
    case UI_VALUE_STRING:
        out(t, " str=%u#%016llx", v->string.length, (unsigned long long)hash(v->string.bytes, v->string.length));
        break;
    default: break;
    }
}

// ---- Generated ---------------------------------------------------------------------------

static void dump_text_style(wire_text *t, const ui_wire_text_style *s) {
    u(t, "family", s->family);
    u(t, "weight", s->weight);
    u(t, "slant", s->slant);
    u(t, "from", s->from_node);
    u(t, "lang", s->language);
    f(t, "size", s->size);
    for (int i = 0; i < 4; i++) f(t, "c", s->color[i]);
}

static void dump_entries(wire_text *t, const uint8_t *p, uint32_t count, bool override) {
    uint32_t off = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = p + off;
        ui_wire_style_entry h;
        ui_wire_style_entry_read(e, UI_WIRE_STYLE_ENTRY_SIZE, &h);
        off += ui_wire_style_entry_size(e);
        out(t, "\n    entry prop=%u kind=%u words=%u", h.property, h.kind, h.words);
        if (!ui_wire_style_prop_known(h.property) || !ui_wire_value_kind_words(h.kind)) {
            out(t, " skipped");
            continue;
        }
        ui_value v;
        bool read = ui_wire_style_value_read(e, &v);
        out(t, " read=%d", read);
        if (!read) continue;
        value(t, &v);
        verdict(t, ui_wire_style_value_valid(h.property, &v, override));
    }
}

// Token declarations: ids, then entries of property 0 holding what a token can hold.
static void dump_token_decls(wire_text *t, const uint8_t *ids, const uint8_t *p, uint32_t count, bool override) {
    uint32_t off = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = p + off;
        ui_wire_style_entry h;
        ui_wire_style_entry_read(e, UI_WIRE_STYLE_ENTRY_SIZE, &h);
        off += ui_wire_style_entry_size(e);
        uint32_t token = ui_wire_get_u32(ids + 4 * i);
        out(t, "\n    token %u kind=%u words=%u", token, h.kind, h.words);
        if (!ui_wire_value_kind_words(h.kind)) {
            out(t, " skipped");
            continue;
        }
        ui_value v;
        bool read = ui_wire_style_value_read(e, &v);
        out(t, " read=%d", read);
        if (!read) continue;
        value(t, &v);
        verdict(t, token && (v.kind == UI_VALUE_UNSET ? override : ui_wire_style_token_value_valid(&v)));
    }
}

#define READ(type, var) \
    ui_wire_##type var; \
    ui_wire_##type##_read(p, op->payload_size, &var)

static void dump_op(wire_text *t, const ui_wire_op *op) {
    const uint8_t *p = op->payload;
    out(t, "\nop %u size %u:", op->opcode, op->payload_size);
    switch (op->opcode) {
    case UI_OP_CREATE_NODE: {
        READ(create_node, v);
        handle(t, "node", v.node);
        handle(t, "parent", v.parent);
        break;
    }
    case UI_OP_DESTROY_NODE: {
        READ(destroy_node, v);
        handle(t, "node", v.node);
        break;
    }
    case UI_OP_SET_RECT: {
        READ(set_rect, v);
        handle(t, "node", v.node);
        f(t, "v", v.x), f(t, "v", v.y), f(t, "v", v.width), f(t, "v", v.height);
        verdict(t, !ui_wire_set_rect_check(&v));
        break;
    }
    case UI_OP_SET_COLOR: {
        READ(set_color, v);
        handle(t, "node", v.node);
        f(t, "v", v.r), f(t, "v", v.g), f(t, "v", v.b), f(t, "v", v.a);
        verdict(t, !ui_wire_set_color_check(&v));
        break;
    }
    case UI_OP_SET_CORNER_RADIUS: {
        READ(set_corner_radius, v);
        handle(t, "node", v.node);
        f(t, "radius", v.radius);
        verdict(t, !ui_wire_set_corner_radius_check(&v));
        break;
    }
    case UI_OP_SET_CLIP: {
        READ(set_clip, v);
        handle(t, "node", v.node);
        u(t, "flags", v.flags);
        verdict(t, !ui_wire_set_clip_check(&v));
        break;
    }
    case UI_OP_START_SPRING: {
        READ(start_spring, v);
        handle(t, "node", v.node);
        u(t, "prop", v.property);
        u(t, "count", v.count);
        f(t, "stiffness", v.stiffness);
        f(t, "damping", v.damping);
        ui_wire_start_spring_tail tail;
        ui_wire_start_spring_tail_of(p, &v, &tail);
        float *target = malloc((v.count ? v.count : 1) * sizeof *target);
        if (!target) abort();
        for (uint32_t i = 0; i < v.count; i++) f(t, "target", target[i] = ui_wire_get_f32(tail.target + 4 * i));
        verdict(t, !ui_wire_start_spring_check(&v, target));
        free(target);
        break;
    }
    case UI_OP_SET_TIMER: {
        READ(set_timer, v);
        u(t, "id", v.id);
        u(t, "deadline", v.deadline_ns);
        break;
    }
    case UI_OP_SET_TEXT: {
        uint32_t header = ui_wire_set_text_header_size(p);
        ui_wire_set_text v;
        ui_wire_set_text_read(p, header, &v);
        ui_wire_set_text_tail tail;
        ui_wire_set_text_tail_of(p, &v, &tail);
        handle(t, "node", v.node);
        u(t, "total", v.text_bytes);
        u(t, "chunk", v.chunk_bytes);
        u(t, "header", header);
        u(t, "spans", v.span_count);
        u(t, "strings", v.string_count);
        u(t, "strings_bytes", v.strings_bytes);
        u(t, "ellipsis", v.ellipsis_bytes);
        u(t, "max_lines", v.max_lines);
        u(t, "dir", v.direction);
        u(t, "flags", v.flags);
        u(t, "stride", tail.spans_stride);
        dump_text_style(t, &v.style);
        f(t, "line_height", v.line_height);
        u(t, "align", v.align);
        u(t, "from_node", v.from_node);
        for (uint32_t i = 0; i < v.span_count; i++) {
            ui_wire_text_span s;
            ui_wire_text_span_read(tail.spans + i * tail.spans_stride, tail.spans_stride, &s);
            out(t, "\n    span");
            u(t, "start", s.start);
            u(t, "end", s.end);
            dump_text_style(t, &s.style);
        }
        region(t, "strings", p, tail.strings, v.strings_bytes);
        region(t, "ellipsis", p, tail.ellipsis, v.ellipsis_bytes);
        region(t, "text", p, tail.text, v.chunk_bytes);
        break;
    }
    case UI_OP_APPEND_TEXT: {
        ui_wire_append_text v;
        ui_wire_append_text_read(p, ui_wire_append_text_header_size(p), &v);
        ui_wire_append_text_tail tail;
        ui_wire_append_text_tail_of(p, &v, &tail);
        handle(t, "node", v.node);
        u(t, "chunk", v.chunk_bytes);
        region(t, "text", p, tail.chunk, v.chunk_bytes);
        break;
    }
    case UI_OP_LOAD_FONT: {
        ui_wire_load_font v;
        ui_wire_load_font_read(p, ui_wire_load_font_header_size(p), &v);
        ui_wire_load_font_tail tail;
        ui_wire_load_font_tail_of(p, &v, &tail);
        u(t, "id", v.id);
        region(t, "path", p, tail.path, v.path_bytes);
        verdict(t, !ui_wire_load_font_check(&v, tail.path));
        break;
    }
    case UI_OP_DEFINE_STYLE: {
        ui_wire_define_style v;
        ui_wire_define_style_read(p, ui_wire_define_style_header_size(p), &v);
        ui_wire_define_style_tail tail;
        ui_wire_define_style_tail_of(p, &v, &tail);
        u(t, "id", v.id);
        u(t, "blocks", v.block_count);
        verdict(t, !ui_wire_define_style_check(&v));
        const uint8_t *bp = tail.blocks;
        for (uint32_t b = 0; b < v.block_count; b++) {
            ui_wire_style_block blk;
            uint32_t bh = ui_wire_style_block_header_size(bp);
            ui_wire_style_block_read(bp, bh, &blk);
            out(t, "\n  block");
            u(t, "bytes", blk.bytes);
            u(t, "states", blk.states);
            u(t, "entries", blk.entry_count);
            u(t, "header", bh);
            ui_wire_style_block_tail bt;
            ui_wire_style_block_tail_of(bp, &blk, &bt);
            dump_entries(t, bt.entries, blk.entry_count, false);
            if (blk.token_count) { // printed only when present: older dumps stay as they were
                out(t, "\n    tokens=%u", blk.token_count);
                dump_token_decls(t, bt.tokens, bt.token_values, blk.token_count, false);
            }
            bp += ui_wire_style_block_size(bp);
        }
        break;
    }
    case UI_OP_DESTROY_STYLE: {
        READ(destroy_style, v);
        u(t, "id", v.id);
        verdict(t, !ui_wire_destroy_style_check(&v));
        break;
    }
    case UI_OP_SET_STYLES: {
        ui_wire_set_styles v;
        ui_wire_set_styles_read(p, ui_wire_set_styles_header_size(p), &v);
        ui_wire_set_styles_tail tail;
        ui_wire_set_styles_tail_of(p, &v, &tail);
        handle(t, "node", v.node);
        u(t, "count", v.count);
        uint32_t *ids = malloc((v.count ? v.count : 1) * sizeof *ids);
        if (!ids) abort();
        for (uint32_t i = 0; i < v.count; i++) u(t, "id", ids[i] = ui_wire_get_u32(tail.ids + 4 * i));
        verdict(t, !ui_wire_set_styles_check(&v, ids));
        free(ids);
        break;
    }
    case UI_OP_SET_PROPS: {
        ui_wire_set_props v;
        ui_wire_set_props_read(p, ui_wire_set_props_header_size(p), &v);
        ui_wire_set_props_tail tail;
        ui_wire_set_props_tail_of(p, &v, &tail);
        handle(t, "node", v.node);
        u(t, "count", v.entry_count);
        dump_entries(t, tail.entries, v.entry_count, true);
        break;
    }
    case UI_OP_SET_TOKEN: {
        READ(set_token, v);
        ui_wire_set_token_tail tail;
        ui_wire_set_token_tail_of(p, &v, &tail);
        ui_wire_style_entry e;
        ui_wire_style_entry_read(tail.entry, UI_WIRE_STYLE_ENTRY_SIZE, &e);
        u(t, "id", v.id);
        out(t, "\n    entry prop=%u kind=%u words=%u", e.property, e.kind, e.words);
        ui_value val;
        bool ok = !ui_wire_set_token_check(&v) && ui_wire_value_kind_words(e.kind) &&
                  ui_wire_style_value_read(tail.entry, &val);
        if (ok) {
            value(t, &val);
            ok = val.kind == UI_VALUE_UNSET || ui_wire_style_token_value_valid(&val);
        }
        verdict(t, ok);
        break;
    }
    case UI_OP_SET_TOKEN_OVERRIDES: {
        ui_wire_set_token_overrides v;
        ui_wire_set_token_overrides_read(p, ui_wire_set_token_overrides_header_size(p), &v);
        ui_wire_set_token_overrides_tail tail;
        ui_wire_set_token_overrides_tail_of(p, &v, &tail);
        handle(t, "node", v.node);
        u(t, "count", v.count);
        dump_token_decls(t, tail.tokens, tail.values, v.count, true);
        break;
    }
    case UI_OP_SET_STATE: {
        READ(set_state, v);
        handle(t, "node", v.node);
        u(t, "mask", v.mask);
        u(t, "value", v.value);
        break;
    }
    case UI_OP_MOVE_NODE: {
        READ(move_node, v);
        handle(t, "node", v.node);
        handle(t, "parent", v.parent);
        handle(t, "before", v.before);
        break;
    }
    case UI_OP_REQUEST_LAYOUT: {
        READ(request_layout, v);
        handle(t, "node", v.node);
        u(t, "id", v.request_id);
        break;
    }
    default: out(t, " unknown"); break;
    }
}

// ---- Buffers -----------------------------------------------------------------------------

void wire_dump_commands(wire_text *t, const uint8_t *data, size_t size) {
    ui_result r = ui_wire_validate_commands(data, size);
    out(t, "commands %zu bytes: %s", size, r == UI_OK ? "valid" : "malformed");
    if (r != UI_OK) return;
    ui_wire_reader rd;
    ui_wire_reader_init(&rd, UI_WIRE_MAGIC_CMD, data, size);
    ui_wire_op op;
    while (ui_wire_reader_next(&rd, &op) == UI_OK) dump_op(t, &op);
    out(t, "\n");
}

// The clock event (7) is newer than the fixtures' dumps, whose reader skipped it as unknown:
// it is renumbered to an unknown type, so this reader skips it too. It has tests of its own.
static uint8_t *without_clock(const uint8_t *data, size_t size) {
    uint8_t *copy = malloc(size ? size : 1);
    if (!copy) abort();
    memcpy(copy, data, size);
    for (size_t at = UI_WIRE_HEADER_SIZE; size >= UI_WIRE_HEADER_SIZE && at + UI_WIRE_OP_HEADER_SIZE <= size;) {
        uint16_t op = ui_wire_get_u16(copy + at), op_size = ui_wire_get_u16(copy + at + 2);
        if (op_size < UI_WIRE_OP_HEADER_SIZE || op_size % 4 || op_size > size - at) break;
        if (op == UI_EVENT_CLOCK) ui_wire_put_u16(copy + at, 0xFFFF);
        at += op_size;
    }
    return copy;
}

static void dump_events(wire_text *t, const uint8_t *data, size_t size) {
    ui_event_reader rd;
    ui_result r = ui_event_reader_init(&rd, data, size);
    out(t, "events %zu bytes: %d", size, r);
    ui_event e;
    while (r == UI_OK && (r = ui_event_next(&rd, &e)) == UI_OK) {
        out(t, "\nevent #%016llx", (unsigned long long)hash(&e, sizeof e));
    }
    out(t, "\nend %d\n", r);
}

void wire_dump_events(wire_text *t, const uint8_t *data, size_t size) {
    uint8_t *copy = without_clock(data, size);
    dump_events(t, copy, size);
    free(copy);
}
