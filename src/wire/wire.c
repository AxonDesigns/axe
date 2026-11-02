#include "wire/wire.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void ui_wire_buf_init(ui_wire_buf *b, uint32_t magic) {
    memset(b, 0, sizeof *b);
    b->magic = magic;
}

void ui_wire_buf_free(ui_wire_buf *b) {
    free(b->data);
    ui_wire_buf_init(b, b->magic);
}

uint8_t *ui_wire_buf_op(ui_wire_buf *b, uint16_t opcode, uint32_t payload) {
    uint32_t op_size = UI_WIRE_OP_HEADER_SIZE + payload;
    if (payload % 4 || op_size > UI_WIRE_MAX_OP_SIZE) return NULL;
    uint32_t start = b->size ? b->size : UI_WIRE_HEADER_SIZE;
    if (start + op_size > UI_WIRE_MAX_BUFFER_SIZE) return NULL;
    uint32_t needed = start + op_size;
    if (needed > b->capacity) {
        uint32_t cap = b->capacity ? b->capacity : 256;
        while (cap < needed) cap *= 2;
        uint8_t *grown = realloc(b->data, cap);
        if (!grown) return NULL;
        b->data = grown;
        b->capacity = cap;
    }
    uint8_t *p = b->data + start;
    p = ui_wire_put_u16(p, opcode);
    p = ui_wire_put_u16(p, (uint16_t)op_size);
    b->size = needed;
    b->op_count++;
    return p;
}

uint8_t *ui_wire_buf_finish(ui_wire_buf *b, size_t *size) {
    if (!b->size) {
        *size = 0;
        return NULL;
    }
    uint8_t *p = b->data;
    p = ui_wire_put_u32(p, b->magic);
    p = ui_wire_put_u16(p, UI_WIRE_VERSION);
    p = ui_wire_put_u16(p, 0); // flags
    p = ui_wire_put_u32(p, b->size);
    ui_wire_put_u32(p, b->op_count);
    uint8_t *out = b->data;
    *size = b->size;
    b->data = NULL;
    b->size = b->capacity = b->op_count = 0;
    return out;
}

ui_result ui_wire_reader_init(ui_wire_reader *r, uint32_t magic, const void *data, size_t size) {
    memset(r, 0, sizeof *r);
    const uint8_t *p = data;
    if (!p || size < UI_WIRE_HEADER_SIZE || size % 4 || size > UI_WIRE_MAX_BUFFER_SIZE)
        return UI_ERR_MALFORMED;
    if (ui_wire_get_u32(p) != magic || ui_wire_get_u16(p + 4) != UI_WIRE_VERSION ||
        ui_wire_get_u32(p + 8) != size)
        return UI_ERR_MALFORMED;
    r->data = p;
    r->size = size;
    r->offset = UI_WIRE_HEADER_SIZE;
    r->op_count = ui_wire_get_u32(p + 12);
    return UI_OK;
}

ui_result ui_wire_reader_next(ui_wire_reader *r, ui_wire_op *op) {
    if (r->offset == r->size) return r->ops_read == r->op_count ? UI_ERR_END : UI_ERR_MALFORMED;
    if (r->size - r->offset < UI_WIRE_OP_HEADER_SIZE) return UI_ERR_MALFORMED;
    const uint8_t *p = r->data + r->offset;
    uint16_t op_size = ui_wire_get_u16(p + 2);
    if (op_size < UI_WIRE_OP_HEADER_SIZE || op_size % 4 || op_size > r->size - r->offset)
        return UI_ERR_MALFORMED;
    if (r->ops_read == r->op_count) return UI_ERR_MALFORMED;
    op->opcode = ui_wire_get_u16(p);
    op->payload_size = (uint16_t)(op_size - UI_WIRE_OP_HEADER_SIZE);
    op->payload = p + UI_WIRE_OP_HEADER_SIZE;
    r->offset += op_size;
    r->ops_read++;
    return UI_OK;
}

uint32_t ui_property_components(ui_property property) {
    switch (property) {
    case UI_PROP_RECT: return 4;
    case UI_PROP_CORNER_RADIUS: return 1;
    case UI_PROP_COLOR: return 4;
    }
    return 0;
}

// ---- Command writer ----------------------------------------------------------------------

struct ui_cmd_writer {
    ui_wire_buf buf;
};

ui_result ui_cmd_writer_create(ui_cmd_writer **out) {
    ui_cmd_writer *w = malloc(sizeof *w);
    if (!w) return UI_ERR_OUT_OF_MEMORY;
    ui_wire_buf_init(&w->buf, UI_WIRE_MAGIC_CMD);
    *out = w;
    return UI_OK;
}

void ui_cmd_writer_destroy(ui_cmd_writer *writer) {
    if (!writer) return;
    ui_wire_buf_free(&writer->buf);
    free(writer);
}

uint32_t ui_cmd_writer_op_count(const ui_cmd_writer *writer) { return writer->buf.op_count; }

static bool all_finite(const float *v, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (!isfinite(v[i])) return false;
    }
    return true;
}

ui_result ui_cmd_create_node(ui_cmd_writer *w, ui_handle node, ui_handle parent) {
    uint8_t *p = ui_wire_buf_op(&w->buf, UI_OP_CREATE_NODE, UI_WIRE_SIZE_CREATE_NODE);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_handle(p, node);
    ui_wire_put_handle(p, parent);
    return UI_OK;
}

ui_result ui_cmd_destroy_node(ui_cmd_writer *w, ui_handle node) {
    uint8_t *p = ui_wire_buf_op(&w->buf, UI_OP_DESTROY_NODE, UI_WIRE_SIZE_DESTROY_NODE);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    ui_wire_put_handle(p, node);
    return UI_OK;
}

ui_result ui_cmd_set_rect(ui_cmd_writer *w, ui_handle node, float x, float y, float width,
                          float height) {
    const float v[4] = {x, y, width, height};
    if (!all_finite(v, 4) || width < 0 || height < 0) return UI_ERR_INVALID_ARGUMENT;
    uint8_t *p = ui_wire_buf_op(&w->buf, UI_OP_SET_RECT, UI_WIRE_SIZE_SET_RECT);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_handle(p, node);
    for (int i = 0; i < 4; i++) p = ui_wire_put_f32(p, v[i]);
    return UI_OK;
}

ui_result ui_cmd_set_corner_radius(ui_cmd_writer *w, ui_handle node, float radius) {
    if (!isfinite(radius) || radius < 0) return UI_ERR_INVALID_ARGUMENT;
    uint8_t *p = ui_wire_buf_op(&w->buf, UI_OP_SET_CORNER_RADIUS, UI_WIRE_SIZE_SET_CORNER_RADIUS);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_handle(p, node);
    ui_wire_put_f32(p, radius);
    return UI_OK;
}

ui_result ui_cmd_set_color(ui_cmd_writer *w, ui_handle node, float r, float g, float b, float a) {
    const float v[4] = {r, g, b, a};
    if (!all_finite(v, 4)) return UI_ERR_INVALID_ARGUMENT;
    uint8_t *p = ui_wire_buf_op(&w->buf, UI_OP_SET_COLOR, UI_WIRE_SIZE_SET_COLOR);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_handle(p, node);
    for (int i = 0; i < 4; i++) p = ui_wire_put_f32(p, v[i]);
    return UI_OK;
}

ui_result ui_cmd_set_clip(ui_cmd_writer *w, ui_handle node, bool clip) {
    uint8_t *p = ui_wire_buf_op(&w->buf, UI_OP_SET_CLIP, UI_WIRE_SIZE_SET_CLIP);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_handle(p, node);
    ui_wire_put_u32(p, clip ? 1u : 0u);
    return UI_OK;
}

ui_result ui_cmd_start_spring(ui_cmd_writer *w, ui_handle node, ui_property property,
                              const float *target, float stiffness, float damping) {
    uint32_t n = ui_property_components(property);
    if (!n || !target || !all_finite(target, n) || !isfinite(stiffness) || stiffness <= 0 ||
        !isfinite(damping) || damping < 0)
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

static ui_result put_timer(ui_cmd_writer *w, uint32_t id, uint64_t deadline_ns) {
    uint8_t *p = ui_wire_buf_op(&w->buf, UI_OP_SET_TIMER, UI_WIRE_SIZE_SET_TIMER);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_u32(p, id);
    p = ui_wire_put_u32(p, 0);
    ui_wire_put_u64(p, deadline_ns);
    return UI_OK;
}

ui_result ui_cmd_set_timer(ui_cmd_writer *w, uint32_t id, uint64_t deadline_ns) {
    if (deadline_ns == UI_WIRE_TIMER_CANCEL) return UI_ERR_INVALID_ARGUMENT;
    return put_timer(w, id, deadline_ns);
}

ui_result ui_cmd_cancel_timer(ui_cmd_writer *w, uint32_t id) {
    return put_timer(w, id, UI_WIRE_TIMER_CANCEL);
}

static bool valid_style(const ui_text_style_desc *st, uint32_t family_count,
                        uint32_t language_count) {
    return (st->family == UI_TEXT_FAMILY_DEFAULT || st->family < family_count) &&
           st->language <= language_count &&
           st->weight >= 1 && st->weight <= 1000 && st->slant <= UI_TEXT_OBLIQUE &&
           isfinite(st->size) && st->size > 0 && st->size <= 4096 && all_finite(st->color, 4);
}

// Languages follow the families in the op's string table; the wire stores string index + 1.
static uint8_t *put_style(uint8_t *p, uint32_t start, uint32_t end, const ui_text_style_desc *st,
                          uint32_t family_count) {
    p = ui_wire_put_u32(p, start);
    p = ui_wire_put_u32(p, end);
    p = ui_wire_put_u16(p, st->family);
    p = ui_wire_put_u16(p, st->weight);
    *p++ = st->slant;
    *p++ = 0;
    p = ui_wire_put_u16(p, st->language ? (uint16_t)(family_count + st->language) : 0);
    p = ui_wire_put_f32(p, st->size);
    for (int i = 0; i < 4; i++) p = ui_wire_put_f32(p, st->color[i]);
    return p;
}

static uint8_t *put_bytes(uint8_t *p, const void *src, uint32_t n) {
    if (n) memcpy(p, src, n);
    memset(p + n, 0, ui_wire_pad4(n) - n);
    return p + ui_wire_pad4(n);
}

ui_result ui_cmd_set_text(ui_cmd_writer *w, ui_handle node, const ui_text_desc *d) {
    if (!d || (d->length && !d->text) || d->length > UI_TEXT_MAX_BYTES ||
        d->span_count > UI_TEXT_MAX_SPANS || (d->span_count && !d->spans) ||
        d->family_count + d->language_count > UI_TEXT_MAX_STRINGS ||
        (d->family_count && !d->families) || (d->language_count && !d->languages) ||
        d->direction > UI_TEXT_DIR_RTL || d->align > UI_TEXT_ALIGN_JUSTIFY ||
        !isfinite(d->line_height) || d->line_height < 0 || d->line_height > 100 ||
        !valid_style(&d->style, d->family_count, d->language_count))
        return UI_ERR_INVALID_ARGUMENT;
    uint32_t prev_end = 0;
    for (uint32_t i = 0; i < d->span_count; i++) {
        const ui_text_span_desc *sp = &d->spans[i];
        if (sp->start >= sp->end || sp->end > d->length || sp->start < prev_end ||
            !valid_style(&sp->style, d->family_count, d->language_count))
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
    p = ui_wire_put_u32(p, chunk);
    p = ui_wire_put_u16(p, (uint16_t)d->span_count);
    p = ui_wire_put_u16(p, (uint16_t)strings);
    p = ui_wire_put_u16(p, (uint16_t)ui_wire_pad4(fam_bytes));
    p = ui_wire_put_u16(p, (uint16_t)ell);
    p = ui_wire_put_u16(p, d->max_lines);
    *p++ = (uint8_t)d->direction;
    *p++ = d->ellipsis ? UI_WIRE_TEXT_CUSTOM_ELLIPSIS : 0;
    p = put_style(p, 0, 0, &d->style, d->family_count);
    p = ui_wire_put_f32(p, d->line_height);
    *p++ = (uint8_t)d->align;
    *p++ = 0;
    *p++ = 0;
    *p++ = 0;
    for (uint32_t i = 0; i < d->span_count; i++)
        p = put_style(p, d->spans[i].start, d->spans[i].end, &d->spans[i].style, d->family_count);
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
        p = ui_wire_put_u32(p, n);
        put_bytes(p, d->text + sent, n);
        sent += n;
    }
    return UI_OK;
}

ui_result ui_cmd_load_font(ui_cmd_writer *w, uint32_t id, const char *path) {
    size_t n = path ? strlen(path) : 0;
    if (!n || n > UI_FONT_PATH_MAX) return UI_ERR_INVALID_ARGUMENT;
    uint8_t *p = ui_wire_buf_op(&w->buf, UI_OP_LOAD_FONT,
                                UI_WIRE_SIZE_LOAD_FONT_HEADER + ui_wire_pad4((uint32_t)n));
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_u32(p, id);
    p = ui_wire_put_u32(p, (uint32_t)n);
    put_bytes(p, path, (uint32_t)n);
    return UI_OK;
}

ui_result ui_cmd_writer_take(ui_cmd_writer *w, uint8_t **out_bytes, size_t *out_size) {
    *out_bytes = ui_wire_buf_finish(&w->buf, out_size);
    return *out_bytes ? UI_OK : UI_ERR_END;
}

// ---- Events --------------------------------------------------------------------------------

ui_result ui_wire_event_frame_committed(ui_wire_buf *b, ui_handle surface, uint64_t frame_id,
                                        uint64_t predicted_present_ns) {
    uint8_t *p = ui_wire_buf_op(b, UI_EVENT_FRAME_COMMITTED, UI_WIRE_SIZE_FRAME_COMMITTED);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_handle(p, surface);
    p = ui_wire_put_u64(p, frame_id);
    ui_wire_put_u64(p, predicted_present_ns);
    return UI_OK;
}

ui_result ui_wire_event_surface_resized(ui_wire_buf *b, ui_handle surface, float width,
                                        float height, uint32_t scale_120) {
    uint8_t *p = ui_wire_buf_op(b, UI_EVENT_SURFACE_RESIZED, UI_WIRE_SIZE_SURFACE_RESIZED);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_handle(p, surface);
    p = ui_wire_put_f32(p, width);
    p = ui_wire_put_f32(p, height);
    ui_wire_put_u32(p, scale_120);
    return UI_OK;
}

ui_result ui_wire_event_surface_close(ui_wire_buf *b, ui_handle surface) {
    uint8_t *p = ui_wire_buf_op(b, UI_EVENT_SURFACE_CLOSE_REQUESTED, UI_WIRE_SIZE_SURFACE_CLOSE);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    ui_wire_put_handle(p, surface);
    return UI_OK;
}

ui_result ui_wire_event_font_loaded(ui_wire_buf *b, uint32_t id, ui_result result) {
    uint8_t *p = ui_wire_buf_op(b, UI_EVENT_FONT_LOADED, UI_WIRE_SIZE_FONT_LOADED);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_u32(p, id);
    ui_wire_put_u32(p, (uint32_t)result);
    return UI_OK;
}

ui_result ui_wire_event_timer_fired(ui_wire_buf *b, uint32_t id, uint64_t deadline_ns,
                                    uint64_t now_ns) {
    uint8_t *p = ui_wire_buf_op(b, UI_EVENT_TIMER_FIRED, UI_WIRE_SIZE_TIMER_FIRED);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p = ui_wire_put_u32(p, id);
    p = ui_wire_put_u32(p, 0);
    p = ui_wire_put_u64(p, deadline_ns);
    ui_wire_put_u64(p, now_ns);
    return UI_OK;
}

ui_result ui_event_reader_init(ui_event_reader *reader, const void *data, size_t size) {
    ui_wire_reader r;
    ui_result res = ui_wire_reader_init(&r, UI_WIRE_MAGIC_EVENT, data, size);
    if (res != UI_OK) return res;
    reader->data = r.data;
    reader->size = r.size;
    reader->offset = r.offset;
    reader->remaining = r.op_count;
    return UI_OK;
}

ui_result ui_event_next(ui_event_reader *reader, ui_event *out) {
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
        switch (op.opcode) {
        case UI_EVENT_FRAME_COMMITTED:
            if (op.payload_size != UI_WIRE_SIZE_FRAME_COMMITTED) return UI_ERR_MALFORMED;
            out->surface = ui_wire_get_handle(op.payload);
            out->frame_committed.frame_id = ui_wire_get_u64(op.payload + 8);
            out->frame_committed.predicted_present_ns = ui_wire_get_u64(op.payload + 16);
            return UI_OK;
        case UI_EVENT_SURFACE_RESIZED:
            if (op.payload_size != UI_WIRE_SIZE_SURFACE_RESIZED) return UI_ERR_MALFORMED;
            out->surface = ui_wire_get_handle(op.payload);
            out->surface_resized.width = ui_wire_get_f32(op.payload + 8);
            out->surface_resized.height = ui_wire_get_f32(op.payload + 12);
            out->surface_resized.scale_120 = ui_wire_get_u32(op.payload + 16);
            return UI_OK;
        case UI_EVENT_SURFACE_CLOSE_REQUESTED:
            if (op.payload_size != UI_WIRE_SIZE_SURFACE_CLOSE) return UI_ERR_MALFORMED;
            out->surface = ui_wire_get_handle(op.payload);
            return UI_OK;
        case UI_EVENT_TIMER_FIRED:
            if (op.payload_size != UI_WIRE_SIZE_TIMER_FIRED) return UI_ERR_MALFORMED;
            out->timer_fired.id = ui_wire_get_u32(op.payload);
            out->timer_fired.deadline_ns = ui_wire_get_u64(op.payload + 8);
            out->timer_fired.now_ns = ui_wire_get_u64(op.payload + 16);
            return UI_OK;
        case UI_EVENT_FONT_LOADED:
            if (op.payload_size != UI_WIRE_SIZE_FONT_LOADED) return UI_ERR_MALFORMED;
            out->font_loaded.id = ui_wire_get_u32(op.payload);
            out->font_loaded.result = (ui_result)ui_wire_get_u32(op.payload + 4);
            return UI_OK;
        default:
            continue; // unknown event from a newer core: skip it
        }
    }
}
