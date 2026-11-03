#include "core/text_op.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "core/color.h"
#include "unicode/utf8.h"

static_assert((int)UI_TEXT_ALIGN_START == (int)UI_ALIGN_START &&
                  (int)UI_TEXT_ALIGN_JUSTIFY == (int)UI_ALIGN_JUSTIFY &&
                  (int)UI_TEXT_ALIGN_CENTER == (int)UI_ALIGN_CENTER,
              "wire and layout alignment values match");

static bool finite4(const float *v) {
    return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]) && isfinite(v[3]);
}

static bool read_style(const uint8_t *p, uint32_t family_count, ui_text_style *out,
                       uint32_t *start, uint32_t *end) {
    *start = ui_wire_get_u32(p);
    *end = ui_wire_get_u32(p + 4);
    uint16_t family = ui_wire_get_u16(p + 8), weight = ui_wire_get_u16(p + 10);
    uint8_t slant = p[12];
    uint16_t language = ui_wire_get_u16(p + 14); // string index + 1; 0 = none
    float size = ui_wire_get_f32(p + 16), c[4];
    for (int i = 0; i < 4; i++) c[i] = ui_wire_get_f32(p + 20 + 4 * i);
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
    uint32_t header = ui_wire_set_text_header(op->payload);
    uint32_t stride = ui_wire_set_text_span(op->payload);
    uint8_t p[UI_WIRE_SIZE_SET_TEXT_HEADER];
    ui_wire_read_fields(p, sizeof p, op->payload, header);
    ui_handle h = ui_wire_get_handle(p);
    *out = (ui_text_op){.node = h};
    uint32_t total = ui_wire_get_u32(p + 8), chunk = ui_wire_get_u16(p + 12);
    uint32_t span_count = ui_wire_get_u16(p + 16), family_count = ui_wire_get_u16(p + 18);
    uint32_t fam_bytes = ui_wire_get_u16(p + 20), ell_len = ui_wire_get_u16(p + 22);
    uint32_t max_lines = ui_wire_get_u16(p + 24);
    uint8_t dir = p[26], flags = p[27];
    float line_height = ui_wire_get_f32(p + 64);
    uint8_t align = p[68];
    const uint8_t *spans_p = op->payload + header;
    const uint8_t *fam_p = spans_p + span_count * stride;
    const uint8_t *ell_p = fam_p + fam_bytes;
    const uint8_t *chunk_p = ell_p + ui_wire_pad4(ell_len);

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
            if (ui_wire_reader_next(&look, &next) != UI_OK || next.opcode != UI_OP_APPEND_TEXT ||
                !ui_handle_eq(ui_wire_get_handle(next.payload), h))
                break;
            uint32_t n = ui_wire_get_u16(next.payload + 8);
            if (n > total - have) break;
            memcpy(t.text + have, next.payload + ui_wire_append_text_header(next.payload), n);
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
    uint32_t a, b;
    if (!why && !read_style(p + 28, family_count, &base, &a, &b)) why = "set_text (style)";
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
        if (!read_style(spans_p + i * stride, family_count, &t.spans[i].style,
                        &t.spans[i].start, &t.spans[i].end) ||
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
    bool custom = flags & UI_WIRE_TEXT_CUSTOM_ELLIPSIS;
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
