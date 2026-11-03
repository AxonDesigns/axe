#include "core/text_op.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "core/color.h"
#include "unicode/utf8.h"
#include "wire/gen/wire_gen.h"

static_assert((int)UI_TEXT_ALIGN_START == (int)UI_ALIGN_START &&
                  (int)UI_TEXT_ALIGN_JUSTIFY == (int)UI_ALIGN_JUSTIFY &&
                  (int)UI_TEXT_ALIGN_CENTER == (int)UI_ALIGN_CENTER,
              "wire and layout alignment values match");

static bool finite4(const float *v) {
    return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]) && isfinite(v[3]);
}

// Fields in `from_node` come from the node and aren't checked; they read as placeholders.
static bool read_style(const ui_wire_text_style *st, uint32_t family_count, uint32_t from_node,
                       ui_text_style *out) {
    uint16_t family = st->family, weight = st->weight;
    uint8_t slant = st->slant;
    uint16_t language = st->language; // string index + 1; 0 = none
    float size = st->size, c[4] = {st->color[0], st->color[1], st->color[2], st->color[3]};
    if (from_node & UI_TEXT_SET_FAMILY) family = UI_TEXT_FAMILY_DEFAULT;
    if (from_node & UI_TEXT_SET_WEIGHT) weight = 400;
    if (from_node & UI_TEXT_SET_SLANT) slant = 0;
    if (from_node & UI_TEXT_SET_LANGUAGE) language = 0;
    if (from_node & UI_TEXT_SET_SIZE) size = 16;
    if (from_node & UI_TEXT_SET_COLOR) c[0] = c[1] = c[2] = 0, c[3] = 1;
    if ((family != UI_TEXT_FAMILY_DEFAULT && family >= family_count) || weight < 1 ||
        weight > 1000 || slant > UI_TEXT_OBLIQUE || !isfinite(size) || size <= 0 ||
        size > 4096 || !finite4(c) || language > family_count)
        return false;
    *out = (ui_text_style){.family = family, .weight = weight, .style = slant, .size = size,
                           .language = language};
    ui_color_from_wire(c, out->color);
    return true;
}

// Maps byte offsets of the received text to the sanitized text. Offsets inside a sequence
// (valid or replaced) snap to its start.
static void map_offsets(const uint8_t *in, uint32_t len, uint32_t *map) {
    uint32_t i = 0, out = 0;
    while (i < len) {
        uint32_t n;
        bool bad = ui_utf8_decode(in + i, len - i, &n) < 0;
        for (uint32_t k = 0; k < n; k++) map[i + k] = out;
        out += bad ? 3 : n;
        i += n;
    }
    map[len] = out;
}

typedef struct text_parts {
    uint8_t *text;      // assembled from set_text and its append_text continuations
    uint8_t *clean;     // sanitized
    ui_text_span *spans;
    char *names;
    uint32_t *name_offsets;
    uint32_t *map;
} text_parts;

static void free_parts(text_parts *t) {
    free(t->text);
    free(t->clean);
    free(t->spans);
    free(t->names);
    free(t->name_offsets);
    free(t->map);
}

void ui_text_op_free(ui_text_op *t) {
    ui_text_source_release(t->source);
    t->source = NULL;
}

void ui_text_op_decode(const ui_wire_op *op, ui_wire_reader *r, ui_text_op *out) {
    // Fields past the header an older writer sent read as 0: line height from the fonts,
    // start alignment.
    ui_wire_set_text v;
    ui_wire_set_text_read(op->payload, ui_wire_set_text_header_size(op->payload), &v);
    ui_wire_set_text_tail tail;
    ui_wire_set_text_tail_of(op->payload, &v, &tail);
    ui_handle h = v.node;
    *out = (ui_text_op){.node = h};
    uint32_t total = v.text_bytes, chunk = v.chunk_bytes;
    uint32_t span_count = v.span_count, family_count = v.string_count;
    uint32_t fam_bytes = v.strings_bytes, ell_len = v.ellipsis_bytes;
    uint32_t max_lines = v.max_lines;
    uint8_t dir = v.direction, flags = v.flags;
    float line_height = v.line_height;
    uint8_t align = v.align;
    // What comes from the node (older writers sent 0: everything is set).
    uint16_t from_node = v.from_node & UI_TEXT_SET_ALL;
    if (from_node & UI_TEXT_SET_LINE_HEIGHT) line_height = 0;
    if (from_node & UI_TEXT_SET_ALIGN) align = 0;
    if (from_node & UI_TEXT_SET_DIRECTION) dir = 0;
    const uint8_t *fam_p = tail.strings, *ell_p = tail.ellipsis, *chunk_p = tail.text;

    text_parts t = {0};
    bool ok = total <= UI_TEXT_MAX_BYTES && chunk <= total;
    if (ok) {
        t.text = malloc(total ? total : 1);
        ok = t.text != NULL;
    }
    uint32_t have = 0;
    if (ok) {
        memcpy(t.text, chunk_p, chunk);
        have = chunk;
        while (have < total) {
            ui_wire_reader look = *r;
            ui_wire_op next;
            if (ui_wire_reader_next(&look, &next) != UI_OK || next.opcode != UI_OP_APPEND_TEXT) break;
            ui_wire_append_text a;
            ui_wire_append_text_read(next.payload, ui_wire_append_text_header_size(next.payload), &a);
            if (!ui_handle_eq(a.node, h)) break;
            uint32_t n = a.chunk_bytes;
            if (n > total - have) break;
            ui_wire_append_text_tail at;
            ui_wire_append_text_tail_of(next.payload, &a, &at);
            memcpy(t.text + have, at.chunk, n);
            have += n;
            *r = look; // the continuation belongs to this set_text
            out->continuations++;
        }
    }
    const char *why = !ok ? "set_text (size)" : have < total ? "set_text (incomplete)" : NULL;
    if (!why && (!isfinite(line_height) || line_height < 0 || line_height > 100 ||
                 align > UI_TEXT_ALIGN_JUSTIFY))
        why = "set_text (paragraph)";
    if (!why && (dir > UI_TEXT_DIR_RTL || span_count > UI_TEXT_MAX_SPANS ||
                 family_count > UI_TEXT_MAX_STRINGS || ell_len > UI_TEXT_MAX_ELLIPSIS))
        why = "set_text (limits)";

    ui_text_style base;
    if (!why && !read_style(&v.style, family_count, from_node, &base)) why = "set_text (style)";
    if (!why) {
        t.spans = malloc((span_count ? span_count : 1) * sizeof *t.spans);
        t.name_offsets = malloc((family_count + 1) * sizeof *t.name_offsets);
        t.names = malloc(fam_bytes ? fam_bytes : 1);
        if (!t.spans || !t.name_offsets || !t.names) {
            why = "set_text (out of memory)";
            out->out_of_memory = true;
        }
    }
    uint32_t prev_end = 0;
    for (uint32_t i = 0; !why && i < span_count; i++) {
        ui_wire_text_span sp;
        ui_wire_text_span_read(tail.spans + i * tail.spans_stride, tail.spans_stride, &sp);
        uint8_t span_from = sp.style.from_node & UI_TEXT_SET_SPAN_ALL;
        t.spans[i].from_node = span_from;
        t.spans[i].start = sp.start;
        t.spans[i].end = sp.end;
        if (!read_style(&sp.style, family_count, span_from, &t.spans[i].style) ||
            t.spans[i].start >= t.spans[i].end || t.spans[i].end > total ||
            t.spans[i].start < prev_end)
            why = "set_text (span)";
        else
            prev_end = t.spans[i].end;
    }
    // Family names: family_count NUL-terminated UTF-8 strings.
    uint32_t pos = 0, names_len = 0;
    for (uint32_t i = 0; !why && i < family_count; i++) {
        const uint8_t *nul = memchr(fam_p + pos, 0, fam_bytes - pos);
        if (!nul || !ui_utf8_valid(fam_p + pos, (size_t)(nul - (fam_p + pos)))) {
            why = "set_text (family names)";
            break;
        }
        uint32_t len = (uint32_t)(nul - (fam_p + pos));
        t.name_offsets[i] = names_len;
        memcpy(t.names + names_len, fam_p + pos, len);
        names_len += len;
        pos += len + 1;
    }
    if (!why) t.name_offsets[family_count] = names_len;
    bool custom = flags & UI_WIRE_TEXT_FLAGS_CUSTOM_ELLIPSIS;
    if (!why && custom && !ui_utf8_valid(ell_p, ell_len)) why = "set_text (ellipsis)";

    // Replace ill-formed UTF-8, keeping span offsets on the same characters.
    size_t clean_len = 0;
    uint32_t replaced = 0;
    if (!why && ui_utf8_sanitize(t.text, total, &t.clean, &clean_len, &replaced) != UI_OK) {
        why = "set_text (out of memory)";
        out->out_of_memory = true;
    }
    if (!why && span_count) {
        t.map = malloc(((size_t)total + 1) * sizeof *t.map);
        if (!t.map) {
            why = "set_text (out of memory)";
            out->out_of_memory = true;
        } else {
            map_offsets(t.text, total, t.map);
            uint32_t kept = 0;
            for (uint32_t i = 0; i < span_count; i++) {
                ui_text_span sp = t.spans[i];
                sp.start = t.map[sp.start];
                sp.end = t.map[sp.end];
                if (sp.start < sp.end) t.spans[kept++] = sp;
            }
            span_count = kept;
        }
    }
    if (!why) {
        ui_paragraph_desc desc = {
            .text = t.clean, .len = (uint32_t)clean_len, .base = base,
            .spans = t.spans, .span_count = span_count,
            .families = t.names, .family_offsets = t.name_offsets, .family_count = family_count,
            .dir = dir == UI_TEXT_DIR_LTR   ? UI_BIDI_LTR
                   : dir == UI_TEXT_DIR_RTL ? UI_BIDI_RTL
                                            : UI_BIDI_AUTO,
            .max_lines = max_lines,
            .ellipsis = custom ? ell_p : (const uint8_t *)"\xE2\x80\xA6",
            .ellipsis_len = custom ? ell_len : 3,
            .align = (ui_align)align, // the wire and layout enums share values
            .line_height = line_height,
            .from_node = from_node,
        };
        if (ui_text_source_create(&desc, &out->source) != UI_OK) {
            why = "set_text (out of memory)";
            out->out_of_memory = true;
        } else {
            out->replaced = replaced;
        }
    }
    out->why = why;
    free_parts(&t);
}
