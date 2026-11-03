#include "wire/wire.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "wire/gen/wire_gen.h"

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

// ---- Command writer ----------------------------------------------------------------------
// The byte layout of every op is generated from idl/wire.idl (wire/gen/wire_gen.h); these
// functions are the C API's argument checks and conveniences on top of it.

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

ui_result ui_cmd_create_node(ui_cmd_writer *w, ui_handle node, ui_handle parent) {
    return ui_wire_create_node_put(&w->buf, &(ui_wire_create_node){node, parent});
}

ui_result ui_cmd_destroy_node(ui_cmd_writer *w, ui_handle node) {
    return ui_wire_destroy_node_put(&w->buf, &(ui_wire_destroy_node){node});
}

ui_result ui_cmd_set_rect(ui_cmd_writer *w, ui_handle node, float x, float y, float width,
                          float height) {
    ui_wire_set_rect v = {node, x, y, width, height};
    return ui_wire_set_rect_check(&v) ? UI_ERR_INVALID_ARGUMENT : ui_wire_set_rect_put(&w->buf, &v);
}

ui_result ui_cmd_set_corner_radius(ui_cmd_writer *w, ui_handle node, float radius) {
    ui_wire_set_corner_radius v = {node, radius};
    if (ui_wire_set_corner_radius_check(&v)) return UI_ERR_INVALID_ARGUMENT;
    return ui_wire_set_corner_radius_put(&w->buf, &v);
}

ui_result ui_cmd_set_color(ui_cmd_writer *w, ui_handle node, float r, float g, float b, float a) {
    ui_wire_set_color v = {node, r, g, b, a};
    return ui_wire_set_color_check(&v) ? UI_ERR_INVALID_ARGUMENT : ui_wire_set_color_put(&w->buf, &v);
}

ui_result ui_cmd_move_node(ui_cmd_writer *w, ui_handle node, ui_handle parent, ui_handle before) {
    return ui_wire_move_node_put(&w->buf, &(ui_wire_move_node){node, parent, before});
}

ui_result ui_cmd_request_layout(ui_cmd_writer *w, ui_handle node, uint32_t request_id) {
    return ui_wire_request_layout_put(&w->buf, &(ui_wire_request_layout){node, request_id});
}

ui_result ui_cmd_set_clip(ui_cmd_writer *w, ui_handle node, bool clip) {
    uint32_t flags = clip ? UI_WIRE_CLIP_FLAGS_CLIP : 0;
    return ui_wire_set_clip_put(&w->buf, &(ui_wire_set_clip){node, flags});
}

ui_result ui_cmd_start_spring(ui_cmd_writer *w, ui_handle node, ui_property property,
                              const float *target, float stiffness, float damping) {
    ui_wire_start_spring v = {node, (uint16_t)property, (uint16_t)ui_property_components(property),
                              stiffness, damping};
    if (!target || ui_wire_start_spring_check(&v, target)) return UI_ERR_INVALID_ARGUMENT;
    return ui_wire_start_spring_put(&w->buf, &v, target);
}

ui_result ui_cmd_set_timer(ui_cmd_writer *w, uint32_t id, uint64_t deadline_ns) {
    if (deadline_ns == UI_WIRE_TIMER_CANCEL) return UI_ERR_INVALID_ARGUMENT;
    return ui_wire_set_timer_put(&w->buf, &(ui_wire_set_timer){id, deadline_ns});
}

ui_result ui_cmd_cancel_timer(ui_cmd_writer *w, uint32_t id) {
    return ui_wire_set_timer_put(&w->buf, &(ui_wire_set_timer){id, UI_WIRE_TIMER_CANCEL});
}

static bool all_finite(const float *v, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (!isfinite(v[i])) return false;
    }
    return true;
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
// which doesn't know `from_node`, then shows) and flagged in `from_node`.
static ui_wire_text_style wire_style(const ui_text_style_desc *st, uint32_t set, uint32_t family_count) {
    static const float black[4] = {0, 0, 0, 1};
    const float *c = set & UI_TEXT_SET_COLOR ? st->color : black;
    return (ui_wire_text_style){
        .family = set & UI_TEXT_SET_FAMILY ? st->family : UI_TEXT_FAMILY_DEFAULT,
        .weight = set & UI_TEXT_SET_WEIGHT ? st->weight : 400,
        .slant = set & UI_TEXT_SET_SLANT ? st->slant : 0,
        .from_node = (uint8_t)(~set & UI_TEXT_SET_SPAN_ALL),
        .language = set & UI_TEXT_SET_LANGUAGE && st->language ? (uint16_t)(family_count + st->language) : 0,
        .size = set & UI_TEXT_SET_SIZE ? st->size : 16,
        .color = {c[0], c[1], c[2], c[3]},
    };
}

ui_result ui_cmd_set_text(ui_cmd_writer *w, ui_handle node, const ui_text_desc *d) {
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
    uint32_t str_bytes = 0, strings = d->family_count + d->language_count;
    for (uint32_t i = 0; i < d->family_count; i++) {
        if (!d->families[i]) return UI_ERR_INVALID_ARGUMENT;
        str_bytes += (uint32_t)strlen(d->families[i]) + 1;
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
        str_bytes += (uint32_t)n + 1;
    }
    uint32_t ell = d->ellipsis ? (uint32_t)strlen(d->ellipsis) : 0;
    if (ell > UI_TEXT_MAX_ELLIPSIS) return UI_ERR_INVALID_ARGUMENT;
    uint32_t region = ui_wire_pad4(str_bytes);
    uint32_t header = UI_WIRE_SET_TEXT_SIZE + d->span_count * UI_WIRE_TEXT_SPAN_SIZE + region + ui_wire_pad4(ell);
    if (header > UI_WIRE_MAX_PAYLOAD || region > UINT16_MAX) return UI_ERR_INVALID_ARGUMENT;
    uint32_t room = (UI_WIRE_MAX_PAYLOAD - header) & ~3u;
    uint32_t chunk = d->length < room ? d->length : room;

    // On failure the writer must be left as it was, without the set_text either.
    uint32_t saved_size = w->buf.size, saved_ops = w->buf.op_count;
    // The string region: families, then languages, each NUL-terminated, zero-padded.
    char local[256], *names = region <= sizeof local ? local : malloc(region);
    ui_wire_text_span local_spans[8], *spans = d->span_count <= 8 ? local_spans : malloc(d->span_count * sizeof *spans);
    ui_result r = names && spans ? UI_OK : UI_ERR_OUT_OF_MEMORY;
    if (r == UI_OK) {
        char *p = names;
        for (uint32_t i = 0; i < strings; i++) {
            const char *str = i < d->family_count ? d->families[i] : d->languages[i - d->family_count];
            size_t n = strlen(str) + 1;
            memcpy(p, str, n);
            p += n;
        }
        memset(p, 0, region - str_bytes);
        for (uint32_t i = 0; i < d->span_count; i++)
            spans[i] = (ui_wire_text_span){d->spans[i].start, d->spans[i].end,
                                           wire_style(&d->spans[i].style, d->spans[i].set, d->family_count)};
        ui_wire_set_text v = {
            .node = node,
            .text_bytes = d->length,
            .chunk_bytes = (uint16_t)chunk,
            .span_count = (uint16_t)d->span_count,
            .string_count = (uint16_t)strings,
            .strings_bytes = (uint16_t)region,
            .ellipsis_bytes = (uint16_t)ell,
            .max_lines = d->max_lines,
            .direction = d->set & UI_TEXT_SET_DIRECTION ? (uint8_t)d->direction : 0,
            .flags = d->ellipsis ? UI_WIRE_TEXT_FLAGS_CUSTOM_ELLIPSIS : 0,
            .style = wire_style(&d->style, d->set, d->family_count),
            .line_height = d->set & UI_TEXT_SET_LINE_HEIGHT ? d->line_height : 0,
            .align = d->set & UI_TEXT_SET_ALIGN ? (uint8_t)d->align : 0,
            .from_node = (uint16_t)(~d->set & UI_TEXT_SET_ALL),
        };
        r = ui_wire_set_text_put(&w->buf, &v, spans, names, d->ellipsis, d->text);
    }
    if (names != local) free(names);
    if (spans != local_spans) free(spans);
    if (r != UI_OK) return r;

    for (uint32_t sent = chunk; sent < d->length;) {
        uint32_t n = d->length - sent;
        uint32_t max = (UI_WIRE_MAX_PAYLOAD - UI_WIRE_APPEND_TEXT_SIZE) & ~3u;
        if (n > max) n = max;
        r = ui_wire_append_text_put(&w->buf, &(ui_wire_append_text){.node = node, .chunk_bytes = (uint16_t)n}, d->text + sent);
        if (r != UI_OK) {
            w->buf.size = saved_size;
            w->buf.op_count = saved_ops;
            return UI_ERR_OUT_OF_MEMORY;
        }
        sent += n;
    }
    return UI_OK;
}

ui_result ui_cmd_load_font(ui_cmd_writer *w, uint32_t id, const char *path) {
    size_t n = path ? strlen(path) : 0;
    if (!n || n > UI_FONT_PATH_MAX) return UI_ERR_INVALID_ARGUMENT;
    return ui_wire_load_font_put(&w->buf, &(ui_wire_load_font){.id = id, .path_bytes = (uint16_t)n}, path);
}

// ---- Styles ------------------------------------------------------------------------------

// Bytes of the entries, or 0 if any declaration is invalid.
static uint64_t decl_bytes(const ui_style_decl *decls, uint32_t count, bool override) {
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!ui_wire_style_value_valid(decls[i].property, &decls[i].value, override)) return 0;
        bytes += 4u * ui_wire_style_value_words(&decls[i].value);
    }
    return count ? bytes : 1; // 1: valid and empty (no bytes are written)
}

ui_result ui_cmd_define_style(ui_cmd_writer *w, uint32_t id, const ui_style_block_desc *blocks,
                              uint32_t block_count) {
    if (!id || block_count > UINT16_MAX || (block_count && !blocks)) return UI_ERR_INVALID_ARGUMENT;
    uint64_t size = 0;
    for (uint32_t b = 0; b < block_count; b++) {
        if (blocks[b].count > UINT16_MAX || (blocks[b].count && !blocks[b].decls))
            return UI_ERR_INVALID_ARGUMENT;
        uint64_t bytes = decl_bytes(blocks[b].decls, blocks[b].count, false);
        if (!bytes) return UI_ERR_INVALID_ARGUMENT;
        bytes = blocks[b].count ? bytes : 0;
        if (UI_WIRE_STYLE_BLOCK_SIZE + bytes > UINT16_MAX) return UI_ERR_INVALID_ARGUMENT;
        size += UI_WIRE_STYLE_BLOCK_SIZE + bytes;
    }
    if (UI_WIRE_DEFINE_STYLE_SIZE + size > UI_WIRE_MAX_PAYLOAD) return UI_ERR_INVALID_ARGUMENT;
    uint8_t *p;
    ui_result r = ui_wire_define_style_put(&w->buf, &(ui_wire_define_style){id, .block_count = (uint16_t)block_count},
                                           (uint32_t)size, &p);
    if (r != UI_OK) return r;
    for (uint32_t b = 0; b < block_count; b++) {
        uint32_t bytes = UI_WIRE_STYLE_BLOCK_SIZE;
        for (uint32_t i = 0; i < blocks[b].count; i++)
            bytes += 4u * ui_wire_style_value_words(&blocks[b].decls[i].value);
        p = ui_wire_style_block_write(p, &(ui_wire_style_block){.bytes = (uint16_t)bytes, .states = blocks[b].states,
                                                                .entry_count = (uint16_t)blocks[b].count});
        for (uint32_t i = 0; i < blocks[b].count; i++)
            p = ui_wire_style_entry_put(p, blocks[b].decls[i].property, &blocks[b].decls[i].value);
    }
    return UI_OK;
}

ui_result ui_cmd_destroy_style(ui_cmd_writer *w, uint32_t id) {
    ui_wire_destroy_style v = {id};
    return ui_wire_destroy_style_check(&v) ? UI_ERR_INVALID_ARGUMENT : ui_wire_destroy_style_put(&w->buf, &v);
}

ui_result ui_cmd_set_styles(ui_cmd_writer *w, ui_handle node, const uint32_t *ids,
                            uint32_t count) {
    ui_wire_set_styles v = {.node = node, .count = (uint16_t)count};
    if (count > UINT8_MAX || (count && !ids) || ui_wire_set_styles_check(&v, ids)) return UI_ERR_INVALID_ARGUMENT;
    return ui_wire_set_styles_put(&w->buf, &v, ids);
}

ui_result ui_cmd_set_props(ui_cmd_writer *w, ui_handle node, const ui_style_decl *decls, uint32_t count) {
    if (count > UINT16_MAX || (count && !decls)) return UI_ERR_INVALID_ARGUMENT;
    uint64_t bytes = decl_bytes(decls, count, true);
    if (!bytes) return UI_ERR_INVALID_ARGUMENT;
    bytes = count ? bytes : 0;
    if (UI_WIRE_SET_PROPS_SIZE + bytes > UI_WIRE_MAX_PAYLOAD) return UI_ERR_INVALID_ARGUMENT;
    uint8_t *p;
    ui_result r = ui_wire_set_props_put(&w->buf, &(ui_wire_set_props){node, .entry_count = (uint16_t)count},
                                        (uint32_t)bytes, &p);
    for (uint32_t i = 0; r == UI_OK && i < count; i++) p = ui_wire_style_entry_put(p, decls[i].property, &decls[i].value);
    return r;
}

static ui_result put_token(ui_cmd_writer *w, uint32_t id, const ui_value *value) {
    if (!id || (value->kind != UI_VALUE_UNSET && !ui_wire_style_token_value_valid(value)))
        return UI_ERR_INVALID_ARGUMENT;
    uint8_t *p;
    ui_result r = ui_wire_set_token_put(&w->buf, &(ui_wire_set_token){id}, 4u * ui_wire_style_value_words(value), &p);
    if (r == UI_OK) ui_wire_style_entry_put(p, 0, value);
    return r;
}

ui_result ui_cmd_set_token(ui_cmd_writer *w, uint32_t id, ui_value value) {
    return put_token(w, id, &value);
}

ui_result ui_cmd_set_state(ui_cmd_writer *w, ui_handle node, uint32_t mask, uint32_t value) {
    return ui_wire_set_state_put(&w->buf, &(ui_wire_set_state){node, mask, value});
}

ui_result ui_cmd_writer_take(ui_cmd_writer *w, uint8_t **out_bytes, size_t *out_size) {
    *out_bytes = ui_wire_buf_finish(&w->buf, out_size);
    return *out_bytes ? UI_OK : UI_ERR_END;
}

// ---- Event reader ------------------------------------------------------------------------

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

static ui_rect rect_of(const ui_wire_rect *r) { return (ui_rect){r->x, r->y, r->width, r->height}; }

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
        // A known event may be longer than this reader expects: a newer core appended
        // fields, and they are ignored. Shorter than its original size is malformed.
        if (!ui_wire_event_framed(op.opcode, op.payload, op.payload_size)) return UI_ERR_MALFORMED;
        memset(out, 0, sizeof *out);
        out->type = (ui_event_type)op.opcode;
        const uint8_t *p = op.payload;
        uint32_t n = op.payload_size;
        switch (op.opcode) {
        case UI_EVENT_FRAME_COMMITTED: {
            ui_wire_frame_committed e;
            ui_wire_frame_committed_read(p, n, &e);
            out->surface = e.surface;
            out->frame_committed.frame_id = e.frame_id;
            out->frame_committed.predicted_present_ns = e.predicted_present_ns;
            return UI_OK;
        }
        case UI_EVENT_SURFACE_RESIZED: {
            ui_wire_surface_resized e;
            ui_wire_surface_resized_read(p, n, &e);
            out->surface = e.surface;
            out->surface_resized.width = e.width;
            out->surface_resized.height = e.height;
            out->surface_resized.scale_120 = e.scale_120;
            return UI_OK;
        }
        case UI_EVENT_SURFACE_CLOSE_REQUESTED: {
            ui_wire_surface_close_requested e;
            ui_wire_surface_close_requested_read(p, n, &e);
            out->surface = e.surface;
            return UI_OK;
        }
        case UI_EVENT_TIMER_FIRED: {
            ui_wire_timer_fired e;
            ui_wire_timer_fired_read(p, n, &e);
            out->timer_fired.id = e.id;
            out->timer_fired.deadline_ns = e.deadline_ns;
            out->timer_fired.now_ns = e.now_ns;
            return UI_OK;
        }
        case UI_EVENT_FONT_LOADED: {
            ui_wire_font_loaded e;
            ui_wire_font_loaded_read(p, n, &e);
            out->font_loaded.id = e.id;
            out->font_loaded.result = (ui_result)e.result;
            return UI_OK;
        }
        case UI_EVENT_LAYOUT_MEASURED: {
            ui_wire_layout_measured e;
            ui_wire_layout_measured_read(p, n, &e);
            out->surface = e.surface;
            out->layout_measured.request_id = e.request_id;
            out->layout_measured.result = (ui_result)e.result;
            out->layout_measured.node = e.node;
            out->layout_measured.layout_in_parent = rect_of(&e.layout_in_parent);
            out->layout_measured.layout_in_surface = rect_of(&e.layout_in_surface);
            out->layout_measured.presentation_in_parent = rect_of(&e.presentation_in_parent);
            out->layout_measured.presentation_in_surface = rect_of(&e.presentation_in_surface);
            return UI_OK;
        }
        case UI_EVENT_CLOCK: {
            ui_wire_clock e;
            ui_wire_clock_read(p, n, &e);
            out->clock.now_ns = e.now_ns;
            return UI_OK;
        }
        default:
            continue; // unknown event from a newer core: skip it
        }
    }
}
