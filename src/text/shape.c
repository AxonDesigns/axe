#include "text/shape.h"

#include <stdlib.h>
#include <string.h>

#include "unicode/script.h"
#include "unicode/ucd.h"
#include "unicode/utf8.h"

void ui_shaped_paragraph_free(ui_shaped_paragraph *p) {
    free(p->cps);
    free(p->offsets);
    free(p->line_breaks);
    free(p->graphemes);
    free(p->styles);
    free(p->runs);
    free(p->glyphs);
    ui_bidi_free(&p->bidi);
    memset(p, 0, sizeof *p);
}

ui_result ui_text_shaper_init(ui_text_shaper *s) {
    memset(s, 0, sizeof *s);
    return ui_shaper_create(&s->hb);
}

void ui_text_shaper_free(ui_text_shaper *s) {
    ui_shaper_destroy(s->hb);
    ui_line_breaker_free(&s->breaker);
    free(s->scripts);
    free(s->faces);
    free(s->style_faces);
    memset(s, 0, sizeof *s);
}

const ui_text_style *ui_text_input_style(const ui_text_input *in, uint32_t index) {
    return index == 0 ? &in->base : &in->spans[index - 1].style;
}

void ui_text_input_family(const ui_text_input *in, const ui_text_style *style,
                          const char **text, uint32_t *len) {
    if (style->family >= in->family_count) {
        *text = "";
        *len = 0;
        return;
    }
    *text = in->families + in->family_offsets[style->family];
    *len = in->family_offsets[style->family + 1] - in->family_offsets[style->family];
}

static bool same_shaping(const ui_text_style *a, const ui_text_style *b) {
    return a->family == b->family && a->weight == b->weight && a->style == b->style &&
           a->size == b->size;
}

// Grows every per-code-point array of the paragraph and the shaper's scratch to n + 1.
static ui_result reserve_cps(ui_text_shaper *s, ui_shaped_paragraph *p, uint32_t n) {
    uint32_t need = n + 1;
    if (need > p->cp_capacity) {
        uint32_t cap = p->cp_capacity;
        if (ui_grow((void **)&p->cps, &cap, need, sizeof *p->cps) != UI_OK) return UI_ERR_OUT_OF_MEMORY;
        cap = p->cp_capacity;
        if (ui_grow((void **)&p->offsets, &cap, need, sizeof *p->offsets) != UI_OK) return UI_ERR_OUT_OF_MEMORY;
        cap = p->cp_capacity;
        if (ui_grow((void **)&p->line_breaks, &cap, need, 1) != UI_OK) return UI_ERR_OUT_OF_MEMORY;
        cap = p->cp_capacity;
        if (ui_grow((void **)&p->graphemes, &cap, need, 1) != UI_OK) return UI_ERR_OUT_OF_MEMORY;
        cap = p->cp_capacity;
        if (ui_grow((void **)&p->styles, &cap, need, sizeof *p->styles) != UI_OK) return UI_ERR_OUT_OF_MEMORY;
        p->cp_capacity = cap;
    }
    if (need > s->scratch_capacity) {
        uint32_t cap = s->scratch_capacity;
        if (ui_grow((void **)&s->scripts, &cap, need, 1) != UI_OK) return UI_ERR_OUT_OF_MEMORY;
        cap = s->scratch_capacity;
        if (ui_grow((void **)&s->faces, &cap, need, sizeof *s->faces) != UI_OK) return UI_ERR_OUT_OF_MEMORY;
        s->scratch_capacity = cap;
    }
    return UI_OK;
}

static ui_result shape_run(ui_text_shaper *s, ui_font_registry *fonts, const ui_text_input *in,
                           ui_shaped_paragraph *p, ui_shaped_run *run, uint32_t face) {
    const ui_text_style *st = ui_text_input_style(in, run->style);
    ui_font *font = ui_font_registry_font(fonts, face, st->size * p->scale, st->weight,
                                          (ui_font_style)st->style, &run->font);
    if (!font) return UI_ERR_OUT_OF_MEMORY;
    ui_font_metrics m;
    ui_font_get_metrics(font, &m);
    run->ascent = m.ascent;
    run->descent = m.descent;
    run->line_gap = m.line_gap;
    ui_shape_request req = {in->text, in->len, run->start, run->end - run->start, run->script,
                            run->level & 1};
    const ui_glyph_pos *glyphs;
    uint32_t count;
    ui_result res = ui_shape(s->hb, font, &req, &glyphs, &count);
    if (res != UI_OK) return res;
    if (ui_grow((void **)&p->glyphs, &p->glyph_capacity, p->glyph_count + count + 1,
                sizeof *p->glyphs) != UI_OK)
        return UI_ERR_OUT_OF_MEMORY;
    memcpy(p->glyphs + p->glyph_count, glyphs, count * sizeof *glyphs);
    run->glyph_start = p->glyph_count;
    run->glyph_count = count;
    p->glyph_count += count;
    return UI_OK;
}

ui_result ui_shape_paragraph(ui_text_shaper *s, ui_font_registry *fonts,
                             const ui_text_input *in, float scale, ui_shaped_paragraph *p) {
    p->scale = scale;
    p->run_count = 0;
    p->glyph_count = 0;
    // Decode. The text is valid UTF-8, so the code point count is at most its length.
    ui_result res = reserve_cps(s, p, in->len);
    if (res != UI_OK) return res;
    uint32_t n = 0;
    for (size_t i = 0; i < in->len;) {
        p->offsets[n] = (uint32_t)i;
        p->cps[n++] = ui_utf8_next(in->text, &i);
    }
    p->offsets[n] = in->len;
    p->cp_count = n;

    res = ui_line_breaks(&s->breaker, p->cps, n, p->line_breaks);
    if (res != UI_OK) return res;
    ui_grapheme_breaks(p->cps, n, p->graphemes);
    res = ui_bidi_resolve(&p->bidi, p->cps, n, in->dir);
    if (res != UI_OK) return res;
    ui_script_runs(p->cps, n, s->scripts);

    // Style per code point.
    uint32_t span = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t off = p->offsets[i];
        while (span < in->span_count && in->spans[span].end <= off) span++;
        p->styles[i] = span < in->span_count && in->spans[span].start <= off ? span + 1 : 0;
    }

    // Primary face per style.
    uint32_t nstyles = in->span_count + 1;
    if (ui_grow((void **)&s->style_faces, &s->style_face_capacity, nstyles,
                sizeof *s->style_faces) != UI_OK)
        return UI_ERR_OUT_OF_MEMORY;
    for (uint32_t k = 0; k < nstyles; k++) {
        const ui_text_style *st = ui_text_input_style(in, k);
        const char *fam;
        uint32_t fam_len;
        ui_text_input_family(in, st, &fam, &fam_len);
        s->style_faces[k] = ui_font_registry_match(fonts, fam, fam_len, st->weight,
                                                   (ui_font_style)st->style);
        if (s->style_faces[k] == UI_NO_FACE) return UI_ERR_WRONG_STATE;
    }

    // Font per grapheme cluster, then runs at cluster starts where anything changes.
    ui_shaped_run cur = {0};
    uint32_t cur_face = UI_NO_FACE;
    bool open = false;
    for (uint32_t g = 0; g < n;) {
        uint32_t end = g + 1;
        while (end < n && !p->graphemes[end]) end++;
        uint32_t style = p->styles[g];
        const ui_text_style *st = ui_text_input_style(in, style);
        const char *fam;
        uint32_t fam_len;
        ui_text_input_family(in, st, &fam, &fam_len);
        // Common and Inherited clusters (spaces, punctuation, digits) stay in the previous
        // cluster's font when it covers them, so a fallback run isn't split at every space.
        uint8_t raw_script = ui_ucd(p->cps[g])->script;
        uint32_t face;
        if (open && (raw_script == UI_SCRIPT_ZYYY || raw_script == UI_SCRIPT_ZINH) &&
            same_shaping(st, ui_text_input_style(in, cur.style)) &&
            ui_font_registry_covers(fonts, cur_face, p->cps + g, end - g))
            face = cur_face;
        else
            face = ui_font_registry_resolve(fonts, s->style_faces[style], fam, fam_len,
                                            st->weight, (ui_font_style)st->style, p->cps + g,
                                            end - g);
        uint8_t level = p->bidi.levels[g];
        uint32_t script = ui_script_tag(s->scripts[g]);
        if (!open || face != cur_face || level != cur.level || script != cur.script ||
            !same_shaping(st, ui_text_input_style(in, cur.style))) {
            if (open) {
                cur.end = p->offsets[g];
                if (ui_grow((void **)&p->runs, &p->run_capacity, p->run_count + 1,
                            sizeof *p->runs) != UI_OK)
                    return UI_ERR_OUT_OF_MEMORY;
                p->runs[p->run_count++] = cur;
                s->faces[p->run_count - 1] = cur_face;
            }
            cur = (ui_shaped_run){.start = p->offsets[g], .script = script, .style = style,
                                  .level = level};
            cur_face = face;
            open = true;
        }
        g = end;
    }
    if (open) {
        cur.end = in->len;
        if (ui_grow((void **)&p->runs, &p->run_capacity, p->run_count + 1, sizeof *p->runs) != UI_OK)
            return UI_ERR_OUT_OF_MEMORY;
        p->runs[p->run_count++] = cur;
        s->faces[p->run_count - 1] = cur_face;
    }
    // s->faces[r] holds run r's face (there are never more runs than code points).
    for (uint32_t r = 0; r < p->run_count; r++) {
        res = shape_run(s, fonts, in, p, &p->runs[r], s->faces[r]);
        if (res != UI_OK) return res;
    }
    return UI_OK;
}

ui_result ui_reshape_range(ui_text_shaper *s, ui_font_registry *fonts, const ui_text_input *in,
                           const ui_shaped_run *run, uint32_t start, uint32_t end,
                           const ui_glyph_pos **glyphs, uint32_t *count) {
    ui_font *font = ui_font_registry_font_by_key(fonts, run->font);
    if (!font) return UI_ERR_OUT_OF_MEMORY;
    ui_shape_request req = {in->text, in->len, start, end - start, run->script, run->level & 1};
    return ui_shape(s->hb, font, &req, glyphs, count);
}

uint32_t ui_shaped_cp_at(const ui_shaped_paragraph *p, uint32_t offset) {
    uint32_t lo = 0, hi = p->cp_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo + 1) / 2;
        if (p->offsets[mid] <= offset) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}
