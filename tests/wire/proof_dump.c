// Canonical dumps of command and event buffers, made twice: with the reads the scene, the
// text decoder and the style decoder did before the IDL (copied here verbatim), and with the
// generated codec. The migration proof (tests/wire/proof.md) requires them to be equal for
// every recorded buffer, every fuzz seed and every fuzzer input.
#include "proof_dump.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/scene.h"
#include "legacy_wire.h"
#include "style/props.h"
#include "wire/gen/wire_gen.h"

// ---- Output ------------------------------------------------------------------------------

[[gnu::format(printf, 2, 3)]] static void out(proof_text *t, const char *fmt, ...) {
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

void proof_text_free(proof_text *t) {
    free(t->data);
    *t = (proof_text){0};
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

static void handle(proof_text *t, const char *name, ui_handle h) { out(t, " %s=%08x:%u", name, h.index, h.generation); }
static void u(proof_text *t, const char *name, uint64_t v) { out(t, " %s=%llu", name, (unsigned long long)v); }
static void f(proof_text *t, const char *name, float v) { out(t, " %s=%08x", name, bits(v)); }
static void region(proof_text *t, const char *name, const uint8_t *base, const uint8_t *p, size_t n) {
    out(t, " %s@%ld:%zu#%016llx", name, (long)(p - base), n, (unsigned long long)hash(p, n));
}
static void verdict(proof_text *t, bool ok) { out(t, " -> %s", ok ? "ok" : "drop"); }

static void value(proof_text *t, const ui_value *v) {
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

// ---- Legacy ------------------------------------------------------------------------------
// The reads below are the pre-IDL ones: offsets, size fields and checks as scene.c,
// text_op.c and style.c had them.

// style.c's read_value.
static bool legacy_read_value(const uint8_t *e, ui_value *v) {
    uint8_t kind = e[2];
    uint32_t words = ui_style_kind_words(kind);
    if (e[3] < words) return false;
    const uint8_t *p = e + UI_WIRE_STYLE_ENTRY_HEADER;
    *v = (ui_value){.kind = (ui_value_kind)kind};
    switch (kind) {
    case UI_VALUE_PX:
    case UI_VALUE_PERCENT:
    case UI_VALUE_NUMBER: v->number = ui_wire_get_f32(p); break;
    case UI_VALUE_KEYWORD: v->keyword = ui_wire_get_u32(p); break;
    case UI_VALUE_TOKEN: v->token = ui_wire_get_u32(p); break;
    case UI_VALUE_SPRING:
        v->spring.stiffness = ui_wire_get_f32(p);
        v->spring.damping = ui_wire_get_f32(p + 4);
        break;
    case UI_VALUE_COLOR:
        for (int i = 0; i < 4; i++) v->color[i] = ui_wire_get_f32(p + 4 * i);
        break;
    case UI_VALUE_STRING:
        v->string.length = ui_wire_get_u16(p);
        v->string.bytes = (const char *)p + 2;
        if (2 + v->string.length > (e[3] - 1u) * 4) return false;
        break;
    default: break;
    }
    return true;
}

static void legacy_entries(proof_text *t, const uint8_t *p, uint32_t count, bool override) {
    uint32_t off = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = p + off;
        off += e[3] * 4u;
        uint16_t prop = ui_wire_get_u16(e);
        out(t, "\n    entry prop=%u kind=%u words=%u", prop, e[2], e[3]);
        if (!ui_style_prop_known(prop) || !ui_style_kind_words(e[2])) {
            out(t, " skipped");
            continue;
        }
        ui_value v;
        bool read = legacy_read_value(e, &v);
        out(t, " read=%d", read);
        if (!read) continue;
        value(t, &v);
        verdict(t, ui_style_value_valid(prop, &v, override));
    }
}

static void legacy_text_style(proof_text *t, const uint8_t *p) {
    u(t, "family", ui_wire_get_u16(p + 8));
    u(t, "weight", ui_wire_get_u16(p + 10));
    u(t, "slant", p[12]);
    u(t, "from", p[13]);
    u(t, "lang", ui_wire_get_u16(p + 14));
    f(t, "size", ui_wire_get_f32(p + 16));
    for (int i = 0; i < 4; i++) f(t, "c", ui_wire_get_f32(p + 20 + 4 * i));
}

static void legacy_op(proof_text *t, const ui_wire_op *op) {
    const uint8_t *p = op->payload;
    out(t, "\nop %u size %u:", op->opcode, op->payload_size);
    switch (op->opcode) {
    case UI_OP_CREATE_NODE:
        handle(t, "node", ui_wire_get_handle(p));
        handle(t, "parent", ui_wire_get_handle(p + 8));
        break;
    case UI_OP_DESTROY_NODE: handle(t, "node", ui_wire_get_handle(p)); break;
    case UI_OP_SET_RECT:
    case UI_OP_SET_COLOR: {
        handle(t, "node", ui_wire_get_handle(p));
        float v[4];
        for (int i = 0; i < 4; i++) f(t, "v", v[i] = ui_wire_get_f32(p + 8 + 4 * i));
        bool ok = isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]) && isfinite(v[3]);
        if (op->opcode == UI_OP_SET_RECT) ok = ok && v[2] >= 0 && v[3] >= 0;
        verdict(t, ok);
        break;
    }
    case UI_OP_SET_CORNER_RADIUS: {
        handle(t, "node", ui_wire_get_handle(p));
        float r = ui_wire_get_f32(p + 8);
        f(t, "radius", r);
        verdict(t, isfinite(r) && r >= 0);
        break;
    }
    case UI_OP_SET_CLIP:
        handle(t, "node", ui_wire_get_handle(p));
        u(t, "flags", ui_wire_get_u32(p + 8));
        verdict(t, !(ui_wire_get_u32(p + 8) & ~1u));
        break;
    case UI_OP_START_SPRING: {
        handle(t, "node", ui_wire_get_handle(p));
        ui_property prop = (ui_property)ui_wire_get_u16(p + 8);
        uint32_t count = ui_wire_get_u16(p + 10);
        float stiffness = ui_wire_get_f32(p + 12), damping = ui_wire_get_f32(p + 16);
        u(t, "prop", prop);
        u(t, "count", count);
        f(t, "stiffness", stiffness);
        f(t, "damping", damping);
        for (uint32_t i = 0; i < count; i++) f(t, "target", ui_wire_get_f32(p + 20 + 4 * i));
        bool ok = count && ui_property_components(prop) == count && isfinite(stiffness) && stiffness > 0 &&
                  isfinite(damping) && damping >= 0;
        for (uint32_t i = 0; ok && i < count; i++) {
            float v = ui_wire_get_f32(p + 20 + 4 * i);
            ok = isfinite(v) && !(prop == UI_PROP_CORNER_RADIUS && v < 0);
        }
        verdict(t, ok);
        break;
    }
    case UI_OP_SET_TIMER:
        u(t, "id", ui_wire_get_u32(p));
        u(t, "deadline", ui_wire_get_u64(p + 8));
        break;
    case UI_OP_SET_TEXT: {
        uint32_t header = ui_wire_set_text_header(p), stride = ui_wire_set_text_span(p);
        uint8_t h[UI_WIRE_SIZE_SET_TEXT_HEADER];
        ui_wire_read_fields(h, sizeof h, p, header);
        handle(t, "node", ui_wire_get_handle(h));
        u(t, "total", ui_wire_get_u32(h + 8));
        u(t, "chunk", ui_wire_get_u16(h + 12));
        u(t, "header", header);
        uint32_t spans = ui_wire_get_u16(h + 16), fam = ui_wire_get_u16(h + 20), ell = ui_wire_get_u16(h + 22);
        u(t, "spans", spans);
        u(t, "strings", ui_wire_get_u16(h + 18));
        u(t, "strings_bytes", fam);
        u(t, "ellipsis", ell);
        u(t, "max_lines", ui_wire_get_u16(h + 24));
        u(t, "dir", h[26]);
        u(t, "flags", h[27]);
        u(t, "stride", stride);
        legacy_text_style(t, h + 28);
        f(t, "line_height", ui_wire_get_f32(h + 64));
        u(t, "align", h[68]);
        u(t, "from_node", ui_wire_get_u16(h + 70));
        const uint8_t *sp = p + header, *fp = sp + spans * stride, *ep = fp + fam;
        const uint8_t *cp = ep + ui_wire_pad4(ell);
        for (uint32_t i = 0; i < spans; i++) {
            out(t, "\n    span");
            u(t, "start", ui_wire_get_u32(sp + i * stride));
            u(t, "end", ui_wire_get_u32(sp + i * stride + 4));
            legacy_text_style(t, sp + i * stride);
        }
        region(t, "strings", p, fp, fam);
        region(t, "ellipsis", p, ep, ell);
        region(t, "text", p, cp, ui_wire_get_u16(h + 12));
        break;
    }
    case UI_OP_APPEND_TEXT: {
        handle(t, "node", ui_wire_get_handle(p));
        uint32_t n = ui_wire_get_u16(p + 8);
        u(t, "chunk", n);
        region(t, "text", p, p + ui_wire_append_text_header(p), n);
        break;
    }
    case UI_OP_LOAD_FONT: {
        u(t, "id", ui_wire_get_u32(p));
        const char *path;
        uint32_t len;
        bool ok = ui_scene_font_path_valid(p, &path, &len);
        region(t, "path", p, (const uint8_t *)path, len);
        verdict(t, ok);
        break;
    }
    case UI_OP_DEFINE_STYLE: {
        uint32_t id = ui_wire_get_u32(p), header = ui_wire_define_style_header(p);
        uint32_t blocks = ui_wire_get_u16(p + 6), off = header;
        u(t, "id", id);
        u(t, "blocks", blocks);
        verdict(t, id != 0);
        for (uint32_t b = 0; b < blocks; b++) {
            const uint8_t *bp = p + off;
            uint32_t bytes = ui_wire_get_u16(bp), bh = ui_wire_size_or(bp + 6, 8);
            out(t, "\n  block");
            u(t, "bytes", bytes);
            u(t, "states", bp[2]);
            u(t, "entries", ui_wire_get_u16(bp + 4));
            u(t, "header", bh);
            legacy_entries(t, bp + bh, ui_wire_get_u16(bp + 4), false);
            off += bytes;
        }
        break;
    }
    case UI_OP_DESTROY_STYLE:
        u(t, "id", ui_wire_get_u32(p));
        verdict(t, ui_wire_get_u32(p) != 0);
        break;
    case UI_OP_SET_STYLES: {
        handle(t, "node", ui_wire_get_handle(p));
        uint32_t count = ui_wire_get_u16(p + 8);
        const uint8_t *ids = p + ui_wire_set_styles_header(p);
        u(t, "count", count);
        bool ok = true;
        for (uint32_t i = 0; i < count; i++) {
            u(t, "id", ui_wire_get_u32(ids + 4 * i));
            if (!ui_wire_get_u32(ids + 4 * i) || i >= UINT8_MAX) ok = false;
        }
        verdict(t, ok);
        break;
    }
    case UI_OP_SET_PROPS: {
        handle(t, "node", ui_wire_get_handle(p));
        uint32_t header = ui_wire_set_props_header(p), count = ui_wire_get_u16(p + 10);
        u(t, "count", count);
        legacy_entries(t, p + header, count, true);
        break;
    }
    case UI_OP_SET_TOKEN: {
        uint32_t id = ui_wire_get_u32(p);
        const uint8_t *e = p + UI_WIRE_SIZE_SET_TOKEN_HEADER;
        u(t, "id", id);
        out(t, "\n    entry prop=%u kind=%u words=%u", ui_wire_get_u16(e), e[2], e[3]);
        ui_value v;
        bool ok = id && ui_style_kind_words(e[2]) && legacy_read_value(e, &v);
        if (ok) {
            value(t, &v);
            ok = v.kind == UI_VALUE_UNSET || ui_style_token_value_valid(&v);
        }
        verdict(t, ok);
        break;
    }
    case UI_OP_SET_STATE:
        handle(t, "node", ui_wire_get_handle(p));
        u(t, "mask", ui_wire_get_u32(p + 8));
        u(t, "value", ui_wire_get_u32(p + 12));
        break;
    case UI_OP_MOVE_NODE:
        handle(t, "node", ui_wire_get_handle(p));
        handle(t, "parent", ui_wire_get_handle(p + 8));
        handle(t, "before", ui_wire_get_handle(p + 16));
        break;
    case UI_OP_REQUEST_LAYOUT:
        handle(t, "node", ui_wire_get_handle(p));
        u(t, "id", ui_wire_get_u32(p + 8));
        break;
    default: out(t, " unknown"); break;
    }
}

// ---- Generated ---------------------------------------------------------------------------

static void gen_text_style(proof_text *t, const ui_wire_text_style *s) {
    u(t, "family", s->family);
    u(t, "weight", s->weight);
    u(t, "slant", s->slant);
    u(t, "from", s->from_node);
    u(t, "lang", s->language);
    f(t, "size", s->size);
    for (int i = 0; i < 4; i++) f(t, "c", s->color[i]);
}

static void gen_entries(proof_text *t, const uint8_t *p, uint32_t count, bool override) {
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

#define READ(type, var) \
    ui_wire_##type var; \
    ui_wire_##type##_read(p, op->payload_size, &var)

static void gen_op(proof_text *t, const ui_wire_op *op) {
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
        gen_text_style(t, &v.style);
        f(t, "line_height", v.line_height);
        u(t, "align", v.align);
        u(t, "from_node", v.from_node);
        for (uint32_t i = 0; i < v.span_count; i++) {
            ui_wire_text_span s;
            ui_wire_text_span_read(tail.spans + i * tail.spans_stride, tail.spans_stride, &s);
            out(t, "\n    span");
            u(t, "start", s.start);
            u(t, "end", s.end);
            gen_text_style(t, &s.style);
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
            gen_entries(t, bt.entries, blk.entry_count, false);
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
        gen_entries(t, tail.entries, v.entry_count, true);
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

static void dump_commands(proof_text *t, const uint8_t *data, size_t size, bool legacy) {
    ui_result r = legacy ? ui_scene_validate(data, size) : ui_wire_validate_commands(data, size);
    out(t, "commands %zu bytes: %s", size, r == UI_OK ? "valid" : "malformed");
    if (r != UI_OK) return;
    ui_wire_reader rd;
    ui_wire_reader_init(&rd, UI_WIRE_MAGIC_CMD, data, size);
    ui_wire_op op;
    while (ui_wire_reader_next(&rd, &op) == UI_OK) (legacy ? legacy_op : gen_op)(t, &op);
    out(t, "\n");
}

// The clock event (7) is new in milestone 3a: the legacy reader skips it as unknown. Both
// readers get the buffer with it renumbered to an unknown type, so both skip it; it has tests
// of its own.
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

static void dump_events(proof_text *t, const uint8_t *data, size_t size, bool legacy) {
    ui_event_reader rd;
    ui_result r = (legacy ? legacy_event_reader_init : ui_event_reader_init)(&rd, data, size);
    out(t, "events %zu bytes: %d", size, r);
    ui_event e;
    while (r == UI_OK && (r = (legacy ? legacy_event_next : ui_event_next)(&rd, &e)) == UI_OK) {
        out(t, "\nevent #%016llx", (unsigned long long)hash(&e, sizeof e));
    }
    out(t, "\nend %d\n", r);
}

void proof_dump_commands(proof_text *legacy, proof_text *generated, const uint8_t *data, size_t size) {
    dump_commands(legacy, data, size, true);
    dump_commands(generated, data, size, false);
}

void proof_dump_events(proof_text *legacy, proof_text *generated, const uint8_t *data, size_t size) {
    uint8_t *copy = without_clock(data, size);
    dump_events(legacy, copy, size, true);
    dump_events(generated, copy, size, false);
    free(copy);
}
