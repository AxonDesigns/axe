// The hand-written encoders and the event reader as they were before the IDL (milestone 3a),
// kept only for the migration proof: tests/wire/proof.c compares them with the generated code
// byte for byte. Built with UI_WIRE_PROOF; removed once the proof is recorded.
#include "legacy_wire.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "style/props.h"

static uint32_t legacy_property_components(ui_property property) {
    switch (property) {
    case UI_PROP_RECT: return 4;
    case UI_PROP_CORNER_RADIUS: return 1;
    case UI_PROP_COLOR: return 4;
    }
    return 0;
}

// ---- Command writer ----------------------------------------------------------------------



ui_result legacy_writer_create(legacy_writer **out) {
    legacy_writer *w = malloc(sizeof *w);
    if (!w) return UI_ERR_OUT_OF_MEMORY;
    ui_wire_buf_init(&w->buf, UI_WIRE_MAGIC_CMD);
    *out = w;
    return UI_OK;
}

void legacy_writer_destroy(legacy_writer *writer) {
    if (!writer) return;
    ui_wire_buf_free(&writer->buf);
    free(writer);
}

uint32_t legacy_writer_op_count(const legacy_writer *writer) { return writer->buf.op_count; }

static bool all_finite(const float *v, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (!isfinite(v[i])) return false;
    }
    return true;
}

ui_result legacy_cmd_create_node(legacy_writer *w, ui_handle node, ui_handle parent) {
    uint8_t *p = ui_wire_buf_op(&w->buf, UI_OP_CREATE_NODE, UI_WIRE_SIZE_CREATE_NODE);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_handle(p, node);
    ui_wire_put_handle(p, parent);
    return UI_OK;
}

ui_result legacy_cmd_destroy_node(legacy_writer *w, ui_handle node) {
    uint8_t *p = ui_wire_buf_op(&w->buf, UI_OP_DESTROY_NODE, UI_WIRE_SIZE_DESTROY_NODE);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    ui_wire_put_handle(p, node);
    return UI_OK;
}

ui_result legacy_cmd_set_rect(legacy_writer *w, ui_handle node, float x, float y, float width,
                          float height) {
    const float v[4] = {x, y, width, height};
    if (!all_finite(v, 4) || width < 0 || height < 0) return UI_ERR_INVALID_ARGUMENT;
    uint8_t *p = ui_wire_buf_op(&w->buf, UI_OP_SET_RECT, UI_WIRE_SIZE_SET_RECT);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_handle(p, node);
    for (int i = 0; i < 4; i++) p = ui_wire_put_f32(p, v[i]);
    return UI_OK;
}

ui_result legacy_cmd_set_corner_radius(legacy_writer *w, ui_handle node, float radius) {
    if (!isfinite(radius) || radius < 0) return UI_ERR_INVALID_ARGUMENT;
    uint8_t *p = ui_wire_buf_op(&w->buf, UI_OP_SET_CORNER_RADIUS, UI_WIRE_SIZE_SET_CORNER_RADIUS);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_handle(p, node);
    ui_wire_put_f32(p, radius);
    return UI_OK;
}

ui_result legacy_cmd_set_color(legacy_writer *w, ui_handle node, float r, float g, float b, float a) {
    const float v[4] = {r, g, b, a};
    if (!all_finite(v, 4)) return UI_ERR_INVALID_ARGUMENT;
    uint8_t *p = ui_wire_buf_op(&w->buf, UI_OP_SET_COLOR, UI_WIRE_SIZE_SET_COLOR);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_handle(p, node);
    for (int i = 0; i < 4; i++) p = ui_wire_put_f32(p, v[i]);
    return UI_OK;
}

ui_result legacy_cmd_move_node(legacy_writer *w, ui_handle node, ui_handle parent, ui_handle before) {
    uint8_t *p = ui_wire_buf_op(&w->buf, UI_OP_MOVE_NODE, UI_WIRE_SIZE_MOVE_NODE);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_handle(p, node);
    p = ui_wire_put_handle(p, parent);
    ui_wire_put_handle(p, before);
    return UI_OK;
}

ui_result legacy_cmd_request_layout(legacy_writer *w, ui_handle node, uint32_t request_id) {
    uint8_t *p = ui_wire_buf_op(&w->buf, UI_OP_REQUEST_LAYOUT, UI_WIRE_SIZE_REQUEST_LAYOUT);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_handle(p, node);
    ui_wire_put_u32(p, request_id);
    return UI_OK;
}

ui_result legacy_cmd_set_clip(legacy_writer *w, ui_handle node, bool clip) {
    uint8_t *p = ui_wire_buf_op(&w->buf, UI_OP_SET_CLIP, UI_WIRE_SIZE_SET_CLIP);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_handle(p, node);
    ui_wire_put_u32(p, clip ? 1u : 0u);
    return UI_OK;
}

ui_result legacy_cmd_start_spring(legacy_writer *w, ui_handle node, ui_property property,
                              const float *target, float stiffness, float damping) {
    uint32_t n = legacy_property_components(property);
    if (!n || !target || !all_finite(target, n) || (property == UI_PROP_CORNER_RADIUS && target[0] < 0) ||
        !isfinite(stiffness) || stiffness <= 0 || !isfinite(damping) || damping < 0)
        return UI_ERR_INVALID_ARGUMENT;
    uint8_t *p = ui_wire_buf_op(&w->buf, UI_OP_START_SPRING, UI_WIRE_SIZE_START_SPRING_BASE + 4 * n);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_handle(p, node);
    p = ui_wire_put_u16(p, (uint16_t)property);
    p = ui_wire_put_u16(p, (uint16_t)n);
    p = ui_wire_put_f32(p, stiffness);
    p = ui_wire_put_f32(p, damping);
    for (uint32_t i = 0; i < n; i++) p = ui_wire_put_f32(p, target[i]);
    return UI_OK;
}

static ui_result legacy_put_timer(legacy_writer *w, uint32_t id, uint64_t deadline_ns) {
    uint8_t *p = ui_wire_buf_op(&w->buf, UI_OP_SET_TIMER, UI_WIRE_SIZE_SET_TIMER);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_u32(p, id);
    p = ui_wire_put_u32(p, 0);
    ui_wire_put_u64(p, deadline_ns);
    return UI_OK;
}

ui_result legacy_cmd_set_timer(legacy_writer *w, uint32_t id, uint64_t deadline_ns) {
    if (deadline_ns == UI_WIRE_TIMER_CANCEL) return UI_ERR_INVALID_ARGUMENT;
    return legacy_put_timer(w, id, deadline_ns);
}

ui_result legacy_cmd_cancel_timer(legacy_writer *w, uint32_t id) {
    return legacy_put_timer(w, id, UI_WIRE_TIMER_CANCEL);
}

// Only the fields in `set` are checked: the others come from the node and aren't sent.
static bool valid_style(const ui_text_style_desc *st, uint32_t set, uint32_t family_count,
                        uint32_t language_count) {
    return (!(set & UI_TEXT_SET_FAMILY) || st->family == UI_TEXT_FAMILY_DEFAULT || st->family < family_count) &&
           (!(set & UI_TEXT_SET_LANGUAGE) || st->language <= language_count) &&
           (!(set & UI_TEXT_SET_WEIGHT) || (st->weight >= 1 && st->weight <= 1000)) &&
           (!(set & UI_TEXT_SET_SLANT) || st->slant <= UI_TEXT_OBLIQUE) &&
           (!(set & UI_TEXT_SET_SIZE) || (isfinite(st->size) && st->size > 0 && st->size <= 4096)) &&
           (!(set & UI_TEXT_SET_COLOR) || all_finite(st->color, 4));
}

// Languages follow the families in the op's string table; the wire stores string index + 1.
// A field that comes from the node is written as a valid placeholder (what an older reader,
// which doesn't know `from_node`, then shows) and flagged in `from_node` (span byte 13).
static uint8_t *put_style(uint8_t *p, uint32_t start, uint32_t end, const ui_text_style_desc *st,
                          uint32_t set, uint32_t family_count) {
    p = ui_wire_put_u32(p, start);
    p = ui_wire_put_u32(p, end);
    p = ui_wire_put_u16(p, set & UI_TEXT_SET_FAMILY ? st->family : UI_TEXT_FAMILY_DEFAULT);
    p = ui_wire_put_u16(p, set & UI_TEXT_SET_WEIGHT ? st->weight : 400);
    *p++ = set & UI_TEXT_SET_SLANT ? st->slant : 0;
    *p++ = (uint8_t)(~set & UI_TEXT_SET_SPAN_ALL);
    p = ui_wire_put_u16(p, set & UI_TEXT_SET_LANGUAGE && st->language ? (uint16_t)(family_count + st->language) : 0);
    p = ui_wire_put_f32(p, set & UI_TEXT_SET_SIZE ? st->size : 16);
    static const float black[4] = {0, 0, 0, 1};
    const float *c = set & UI_TEXT_SET_COLOR ? st->color : black;
    for (int i = 0; i < 4; i++) p = ui_wire_put_f32(p, c[i]);
    return p;
}

static uint8_t *put_bytes(uint8_t *p, const void *src, uint32_t n) {
    if (n) memcpy(p, src, n);
    memset(p + n, 0, ui_wire_pad4(n) - n);
    return p + ui_wire_pad4(n);
}

ui_result legacy_cmd_set_text(legacy_writer *w, ui_handle node, const ui_text_desc *d) {
    if (!d || (d->length && !d->text) || d->length > UI_TEXT_MAX_BYTES ||
        d->span_count > UI_TEXT_MAX_SPANS || (d->span_count && !d->spans) ||
        d->family_count + d->language_count > UI_TEXT_MAX_STRINGS ||
        (d->family_count && !d->families) || (d->language_count && !d->languages) ||
        d->direction > UI_TEXT_DIR_RTL || d->align > UI_TEXT_ALIGN_JUSTIFY ||
        !isfinite(d->line_height) || d->line_height < 0 || d->line_height > 100 ||
        (d->set & ~(uint32_t)UI_TEXT_SET_ALL) ||
        !valid_style(&d->style, d->set, d->family_count, d->language_count))
        return UI_ERR_INVALID_ARGUMENT;
    uint32_t prev_end = 0;
    for (uint32_t i = 0; i < d->span_count; i++) {
        const ui_text_span_desc *sp = &d->spans[i];
        if (sp->start >= sp->end || sp->end > d->length || sp->start < prev_end ||
            (sp->set & ~(uint32_t)UI_TEXT_SET_SPAN_ALL) ||
            !valid_style(&sp->style, sp->set, d->family_count, d->language_count))
            return UI_ERR_INVALID_ARGUMENT;
        prev_end = sp->end;
    }
    uint32_t fam_bytes = 0, strings = d->family_count + d->language_count;
    for (uint32_t i = 0; i < d->family_count; i++) {
        if (!d->families[i]) return UI_ERR_INVALID_ARGUMENT;
        fam_bytes += (uint32_t)strlen(d->families[i]) + 1;
    }
    for (uint32_t i = 0; i < d->language_count; i++) {
        const char *tag = d->languages[i];
        size_t n = tag ? strlen(tag) : 0;
        if (!n || n > UI_TEXT_MAX_LANGUAGE) return UI_ERR_INVALID_ARGUMENT;
        for (size_t k = 0; k < n; k++) {
            char ch = tag[k];
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                  ch == '-'))
                return UI_ERR_INVALID_ARGUMENT;
        }
        fam_bytes += (uint32_t)n + 1;
    }
    uint32_t ell = d->ellipsis ? (uint32_t)strlen(d->ellipsis) : 0;
    if (ell > UI_TEXT_MAX_ELLIPSIS) return UI_ERR_INVALID_ARGUMENT;
    uint32_t header = UI_WIRE_SIZE_SET_TEXT_HEADER + d->span_count * UI_WIRE_SIZE_TEXT_SPAN +
                      ui_wire_pad4(fam_bytes) + ui_wire_pad4(ell);
    if (header > UI_WIRE_MAX_PAYLOAD || ui_wire_pad4(fam_bytes) > UINT16_MAX)
        return UI_ERR_INVALID_ARGUMENT;
    uint32_t room = (UI_WIRE_MAX_PAYLOAD - header) & ~3u;
    uint32_t chunk = d->length < room ? d->length : room;

    // On failure the writer must be left as it was: remember where it stood.
    uint32_t saved_size = w->buf.size, saved_ops = w->buf.op_count;
    uint8_t *p = ui_wire_buf_op(&w->buf, UI_OP_SET_TEXT, header + ui_wire_pad4(chunk));
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_handle(p, node);
    p = ui_wire_put_u32(p, d->length);
    p = ui_wire_put_u16(p, (uint16_t)chunk);
    p = ui_wire_put_u16(p, UI_WIRE_SIZE_SET_TEXT_HEADER);
    p = ui_wire_put_u16(p, (uint16_t)d->span_count);
    p = ui_wire_put_u16(p, (uint16_t)strings);
    p = ui_wire_put_u16(p, (uint16_t)ui_wire_pad4(fam_bytes));
    p = ui_wire_put_u16(p, (uint16_t)ell);
    p = ui_wire_put_u16(p, d->max_lines);
    *p++ = d->set & UI_TEXT_SET_DIRECTION ? (uint8_t)d->direction : 0;
    *p++ = d->ellipsis ? UI_WIRE_TEXT_CUSTOM_ELLIPSIS : 0;
    // The default style's start and end are unused; the first two bytes carry span_bytes.
    // Its byte 13 holds the span bits of from_node; the rest of the mask is at 70.
    p = put_style(p, UI_WIRE_SIZE_TEXT_SPAN, 0, &d->style, d->set, d->family_count);
    p = ui_wire_put_f32(p, d->set & UI_TEXT_SET_LINE_HEIGHT ? d->line_height : 0);
    *p++ = d->set & UI_TEXT_SET_ALIGN ? (uint8_t)d->align : 0;
    *p++ = 0;
    ui_wire_put_u16(p, (uint16_t)(~d->set & UI_TEXT_SET_ALL)); // from_node
    p += 2;
    for (uint32_t i = 0; i < d->span_count; i++)
        p = put_style(p, d->spans[i].start, d->spans[i].end, &d->spans[i].style, d->spans[i].set,
                      d->family_count);
    uint8_t *names = p;
    for (uint32_t i = 0; i < strings; i++) {
        const char *str = i < d->family_count ? d->families[i] : d->languages[i - d->family_count];
        size_t n = strlen(str) + 1;
        memcpy(p, str, n);
        p += n;
    }
    memset(p, 0, ui_wire_pad4(fam_bytes) - fam_bytes);
    p = names + ui_wire_pad4(fam_bytes);
    p = put_bytes(p, d->ellipsis, ell);
    put_bytes(p, d->text, chunk);

    for (uint32_t sent = chunk; sent < d->length;) {
        uint32_t n = d->length - sent;
        uint32_t max = (UI_WIRE_MAX_PAYLOAD - UI_WIRE_SIZE_APPEND_TEXT_HEADER) & ~3u;
        if (n > max) n = max;
        p = ui_wire_buf_op(&w->buf, UI_OP_APPEND_TEXT, UI_WIRE_SIZE_APPEND_TEXT_HEADER + ui_wire_pad4(n));
        if (!p) {
            w->buf.size = saved_size;
            w->buf.op_count = saved_ops;
            return UI_ERR_OUT_OF_MEMORY;
        }
        p = ui_wire_put_handle(p, node);
        p = ui_wire_put_u16(p, (uint16_t)n);
        p = ui_wire_put_u16(p, UI_WIRE_SIZE_APPEND_TEXT_HEADER);
        put_bytes(p, d->text + sent, n);
        sent += n;
    }
    return UI_OK;
}

ui_result legacy_cmd_load_font(legacy_writer *w, uint32_t id, const char *path) {
    size_t n = path ? strlen(path) : 0;
    if (!n || n > UI_FONT_PATH_MAX) return UI_ERR_INVALID_ARGUMENT;
    uint8_t *p = ui_wire_buf_op(&w->buf, UI_OP_LOAD_FONT,
                                UI_WIRE_SIZE_LOAD_FONT_HEADER + ui_wire_pad4((uint32_t)n));
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_u32(p, id);
    p = ui_wire_put_u16(p, (uint16_t)n);
    p = ui_wire_put_u16(p, UI_WIRE_SIZE_LOAD_FONT_HEADER);
    put_bytes(p, path, (uint32_t)n);
    return UI_OK;
}

// ---- Styles ------------------------------------------------------------------------------

static uint8_t *put_value(uint8_t *p, const ui_value *v) {
    switch (v->kind) {
    case UI_VALUE_PX:
    case UI_VALUE_PERCENT:
    case UI_VALUE_NUMBER: return ui_wire_put_f32(p, v->number);
    case UI_VALUE_KEYWORD: return ui_wire_put_u32(p, v->keyword);
    case UI_VALUE_TOKEN: return ui_wire_put_u32(p, v->token);
    case UI_VALUE_SPRING:
        p = ui_wire_put_f32(p, v->spring.stiffness);
        return ui_wire_put_f32(p, v->spring.damping);
    case UI_VALUE_COLOR:
        for (int i = 0; i < 4; i++) p = ui_wire_put_f32(p, v->color[i]);
        return p;
    case UI_VALUE_STRING: {
        p = ui_wire_put_u16(p, (uint16_t)v->string.length);
        memcpy(p, v->string.bytes, v->string.length);
        uint32_t padded = (2 + v->string.length + 3) / 4 * 4 - 2;
        memset(p + v->string.length, 0, padded - v->string.length);
        return p + padded;
    }
    default: return p;
    }
}

static uint8_t *put_entry(uint8_t *p, uint16_t prop, const ui_value *v) {
    uint32_t words = ui_style_value_words(v);
    p = ui_wire_put_u16(p, prop);
    *p++ = (uint8_t)v->kind;
    *p++ = (uint8_t)words;
    return put_value(p, v);
}

// Bytes of the entries, or 0 if any declaration is invalid.
static uint64_t decl_bytes(const ui_style_decl *decls, uint32_t count, bool override) {
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!ui_style_value_valid(decls[i].property, &decls[i].value, override)) return 0;
        bytes += 4u * ui_style_value_words(&decls[i].value);
    }
    return count ? bytes : 1; // 1: valid and empty (no bytes are written)
}

ui_result legacy_cmd_define_style(legacy_writer *w, uint32_t id, const ui_style_block_desc *blocks,
                              uint32_t block_count) {
    if (!id || block_count > UINT16_MAX || (block_count && !blocks)) return UI_ERR_INVALID_ARGUMENT;
    uint64_t size = UI_WIRE_SIZE_DEFINE_STYLE_HEADER;
    for (uint32_t b = 0; b < block_count; b++) {
        if (blocks[b].count > UINT16_MAX || (blocks[b].count && !blocks[b].decls))
            return UI_ERR_INVALID_ARGUMENT;
        uint64_t bytes = decl_bytes(blocks[b].decls, blocks[b].count, false);
        if (!bytes) return UI_ERR_INVALID_ARGUMENT;
        bytes = blocks[b].count ? bytes : 0;
        if (UI_WIRE_SIZE_STYLE_BLOCK_HEADER + bytes > UINT16_MAX) return UI_ERR_INVALID_ARGUMENT;
        size += UI_WIRE_SIZE_STYLE_BLOCK_HEADER + bytes;
    }
    if (size > UI_WIRE_MAX_PAYLOAD) return UI_ERR_INVALID_ARGUMENT;
    uint8_t *p = ui_wire_buf_op(&w->buf, UI_OP_DEFINE_STYLE, (uint32_t)size);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_u32(p, id);
    p = ui_wire_put_u16(p, UI_WIRE_SIZE_DEFINE_STYLE_HEADER);
    p = ui_wire_put_u16(p, (uint16_t)block_count);
    for (uint32_t b = 0; b < block_count; b++) {
        uint32_t bytes = UI_WIRE_SIZE_STYLE_BLOCK_HEADER;
        for (uint32_t i = 0; i < blocks[b].count; i++)
            bytes += 4u * ui_style_value_words(&blocks[b].decls[i].value);
        p = ui_wire_put_u16(p, (uint16_t)bytes);
        *p++ = blocks[b].states;
        *p++ = 0;
        p = ui_wire_put_u16(p, (uint16_t)blocks[b].count);
        p = ui_wire_put_u16(p, 0);
        for (uint32_t i = 0; i < blocks[b].count; i++)
            p = put_entry(p, blocks[b].decls[i].property, &blocks[b].decls[i].value);
    }
    return UI_OK;
}

ui_result legacy_cmd_destroy_style(legacy_writer *w, uint32_t id) {
    if (!id) return UI_ERR_INVALID_ARGUMENT;
    uint8_t *p = ui_wire_buf_op(&w->buf, UI_OP_DESTROY_STYLE, UI_WIRE_SIZE_DESTROY_STYLE);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    ui_wire_put_u32(p, id);
    return UI_OK;
}

ui_result legacy_cmd_set_styles(legacy_writer *w, ui_handle node, const uint32_t *ids,
                            uint32_t count) {
    if (count > UINT8_MAX || (count && !ids)) return UI_ERR_INVALID_ARGUMENT;
    for (uint32_t i = 0; i < count; i++) {
        if (!ids[i]) return UI_ERR_INVALID_ARGUMENT;
    }
    uint8_t *p = ui_wire_buf_op(&w->buf, UI_OP_SET_STYLES, UI_WIRE_SIZE_SET_STYLES_HEADER + 4 * count);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_handle(p, node);
    p = ui_wire_put_u16(p, (uint16_t)count);
    p = ui_wire_put_u16(p, UI_WIRE_SIZE_SET_STYLES_HEADER);
    for (uint32_t i = 0; i < count; i++) p = ui_wire_put_u32(p, ids[i]);
    return UI_OK;
}

ui_result legacy_cmd_set_props(legacy_writer *w, ui_handle node, const ui_style_decl *decls,
                           uint32_t count) {
    if (count > UINT16_MAX || (count && !decls)) return UI_ERR_INVALID_ARGUMENT;
    uint64_t bytes = decl_bytes(decls, count, true);
    if (!bytes) return UI_ERR_INVALID_ARGUMENT;
    bytes = count ? bytes : 0;
    if (UI_WIRE_SIZE_SET_PROPS_HEADER + bytes > UI_WIRE_MAX_PAYLOAD) return UI_ERR_INVALID_ARGUMENT;
    uint8_t *p = ui_wire_buf_op(&w->buf, UI_OP_SET_PROPS,
                                (uint32_t)(UI_WIRE_SIZE_SET_PROPS_HEADER + bytes));
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_handle(p, node);
    p = ui_wire_put_u16(p, UI_WIRE_SIZE_SET_PROPS_HEADER);
    p = ui_wire_put_u16(p, (uint16_t)count);
    for (uint32_t i = 0; i < count; i++) p = put_entry(p, decls[i].property, &decls[i].value);
    return UI_OK;
}

ui_result legacy_cmd_set_token(legacy_writer *w, uint32_t id, ui_value value) {
    if (!id || (value.kind != UI_VALUE_UNSET && !ui_style_token_value_valid(&value)))
        return UI_ERR_INVALID_ARGUMENT;
    uint32_t words = ui_style_value_words(&value);
    uint8_t *p = ui_wire_buf_op(&w->buf, UI_OP_SET_TOKEN, UI_WIRE_SIZE_SET_TOKEN_HEADER + 4 * words);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_u32(p, id);
    put_entry(p, 0, &value);
    return UI_OK;
}

ui_result legacy_cmd_set_state(legacy_writer *w, ui_handle node, uint32_t mask, uint32_t value) {
    uint8_t *p = ui_wire_buf_op(&w->buf, UI_OP_SET_STATE, UI_WIRE_SIZE_SET_STATE);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_handle(p, node);
    p = ui_wire_put_u32(p, mask);
    ui_wire_put_u32(p, value);
    return UI_OK;
}

ui_result legacy_writer_take(legacy_writer *w, uint8_t **out_bytes, size_t *out_size) {
    *out_bytes = ui_wire_buf_finish(&w->buf, out_size);
    return *out_bytes ? UI_OK : UI_ERR_END;
}

// ---- Events --------------------------------------------------------------------------------

ui_result legacy_event_frame_committed(ui_wire_buf *b, ui_handle surface, uint64_t frame_id,
                                        uint64_t predicted_present_ns) {
    uint8_t *p = ui_wire_buf_op(b, UI_EVENT_FRAME_COMMITTED, UI_WIRE_SIZE_FRAME_COMMITTED);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_handle(p, surface);
    p = ui_wire_put_u64(p, frame_id);
    ui_wire_put_u64(p, predicted_present_ns);
    return UI_OK;
}

ui_result legacy_event_surface_resized(ui_wire_buf *b, ui_handle surface, float width,
                                        float height, uint32_t scale_120) {
    uint8_t *p = ui_wire_buf_op(b, UI_EVENT_SURFACE_RESIZED, UI_WIRE_SIZE_SURFACE_RESIZED);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_handle(p, surface);
    p = ui_wire_put_f32(p, width);
    p = ui_wire_put_f32(p, height);
    ui_wire_put_u32(p, scale_120);
    return UI_OK;
}

ui_result legacy_event_surface_close(ui_wire_buf *b, ui_handle surface) {
    uint8_t *p = ui_wire_buf_op(b, UI_EVENT_SURFACE_CLOSE_REQUESTED, UI_WIRE_SIZE_SURFACE_CLOSE);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    ui_wire_put_handle(p, surface);
    return UI_OK;
}

ui_result legacy_event_font_loaded(ui_wire_buf *b, uint32_t id, ui_result result) {
    uint8_t *p = ui_wire_buf_op(b, UI_EVENT_FONT_LOADED, UI_WIRE_SIZE_FONT_LOADED);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_u32(p, id);
    ui_wire_put_u32(p, (uint32_t)result);
    return UI_OK;
}

ui_result legacy_event_layout_measured(ui_wire_buf *b, uint32_t request_id, ui_handle node,
                                        ui_handle surface, ui_result result, const float rects[4][4]) {
    uint8_t *p = ui_wire_buf_op(b, UI_EVENT_LAYOUT_MEASURED, UI_WIRE_SIZE_LAYOUT_MEASURED);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_u32(p, request_id);
    p = ui_wire_put_u32(p, (uint32_t)result);
    p = ui_wire_put_handle(p, node);
    p = ui_wire_put_handle(p, surface);
    for (int r = 0; r < 4; r++)
        for (int i = 0; i < 4; i++) p = ui_wire_put_f32(p, rects[r][i]);
    return UI_OK;
}

ui_result legacy_event_timer_fired(ui_wire_buf *b, uint32_t id, uint64_t deadline_ns,
                                    uint64_t now_ns) {
    uint8_t *p = ui_wire_buf_op(b, UI_EVENT_TIMER_FIRED, UI_WIRE_SIZE_TIMER_FIRED);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_u32(p, id);
    p = ui_wire_put_u32(p, 0);
    p = ui_wire_put_u64(p, deadline_ns);
    ui_wire_put_u64(p, now_ns);
    return UI_OK;
}

ui_result legacy_event_reader_init(ui_event_reader *reader, const void *data, size_t size) {
    ui_wire_reader r;
    ui_result res = ui_wire_reader_init(&r, UI_WIRE_MAGIC_EVENT, data, size);
    if (res != UI_OK) return res;
    reader->data = r.data;
    reader->size = r.size;
    reader->offset = r.offset;
    reader->remaining = r.op_count;
    return UI_OK;
}

ui_result legacy_event_next(ui_event_reader *reader, ui_event *out) {
    // Resume a wire reader at the public reader's cursor.
    ui_wire_reader r = {.data = reader->data, .size = reader->size, .offset = reader->offset,
                        .op_count = reader->remaining, .ops_read = 0};
    ui_wire_op op;
    for (;;) {
        ui_result res = ui_wire_reader_next(&r, &op);
        if (res != UI_OK) return res;
        reader->offset = r.offset;
        reader->remaining--;
        memset(out, 0, sizeof *out);
        out->type = (ui_event_type)op.opcode;
        // A known event may be longer than this reader expects: a newer core appended
        // fields, and they are ignored. Shorter than its original size is malformed.
        switch (op.opcode) {
        case UI_EVENT_FRAME_COMMITTED:
            if (op.payload_size < UI_WIRE_SIZE_FRAME_COMMITTED) return UI_ERR_MALFORMED;
            out->surface = ui_wire_get_handle(op.payload);
            out->frame_committed.frame_id = ui_wire_get_u64(op.payload + 8);
            out->frame_committed.predicted_present_ns = ui_wire_get_u64(op.payload + 16);
            return UI_OK;
        case UI_EVENT_SURFACE_RESIZED:
            if (op.payload_size < UI_WIRE_SIZE_SURFACE_RESIZED) return UI_ERR_MALFORMED;
            out->surface = ui_wire_get_handle(op.payload);
            out->surface_resized.width = ui_wire_get_f32(op.payload + 8);
            out->surface_resized.height = ui_wire_get_f32(op.payload + 12);
            out->surface_resized.scale_120 = ui_wire_get_u32(op.payload + 16);
            return UI_OK;
        case UI_EVENT_SURFACE_CLOSE_REQUESTED:
            if (op.payload_size < UI_WIRE_SIZE_SURFACE_CLOSE) return UI_ERR_MALFORMED;
            out->surface = ui_wire_get_handle(op.payload);
            return UI_OK;
        case UI_EVENT_TIMER_FIRED:
            if (op.payload_size < UI_WIRE_SIZE_TIMER_FIRED) return UI_ERR_MALFORMED;
            out->timer_fired.id = ui_wire_get_u32(op.payload);
            out->timer_fired.deadline_ns = ui_wire_get_u64(op.payload + 8);
            out->timer_fired.now_ns = ui_wire_get_u64(op.payload + 16);
            return UI_OK;
        case UI_EVENT_FONT_LOADED:
            if (op.payload_size < UI_WIRE_SIZE_FONT_LOADED) return UI_ERR_MALFORMED;
            out->font_loaded.id = ui_wire_get_u32(op.payload);
            out->font_loaded.result = (ui_result)ui_wire_get_u32(op.payload + 4);
            return UI_OK;
        case UI_EVENT_LAYOUT_MEASURED: {
            if (op.payload_size < UI_WIRE_SIZE_LAYOUT_MEASURED) return UI_ERR_MALFORMED;
            out->layout_measured.request_id = ui_wire_get_u32(op.payload);
            out->layout_measured.result = (ui_result)ui_wire_get_u32(op.payload + 4);
            out->layout_measured.node = ui_wire_get_handle(op.payload + 8);
            out->surface = ui_wire_get_handle(op.payload + 16);
            ui_rect *rects[4] = {&out->layout_measured.layout_in_parent, &out->layout_measured.layout_in_surface,
                                 &out->layout_measured.presentation_in_parent,
                                 &out->layout_measured.presentation_in_surface};
            for (int k = 0; k < 4; k++) {
                const uint8_t *q = op.payload + 24 + 16 * k;
                *rects[k] = (ui_rect){ui_wire_get_f32(q), ui_wire_get_f32(q + 4), ui_wire_get_f32(q + 8),
                                      ui_wire_get_f32(q + 12)};
            }
            return UI_OK;
        }
        default:
            continue; // unknown event from a newer core: skip it
        }
    }
}
