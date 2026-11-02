#include "text/paragraph.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "unicode/ucd.h"

#define EPS (1.f / 64.f) // widths within a 26.6 unit count as fitting
#define NO_RUN UINT32_MAX

typedef struct ui_piece {
    uint32_t start, end;   // bytes
    uint32_t run;          // shaped run giving the font and metrics
    uint8_t level;
    bool ellipsis;
    uint32_t glyph_start, glyph_count;
} ui_piece;

// ---- Lifetime ----------------------------------------------------------------------------

void ui_paragraph_layout_free(ui_paragraph_layout *l) {
    free(l->lines);
    free(l->runs);
    free(l->glyphs);
    memset(l, 0, sizeof *l);
}

ui_result ui_text_context_init(ui_text_context *c, ui_font_registry *fonts) {
    memset(c, 0, sizeof *c);
    c->fonts = fonts;
    return ui_text_shaper_init(&c->shaper);
}

void ui_text_context_free(ui_text_context *c) {
    ui_text_shaper_free(&c->shaper);
    free(c->advances);
    free(c->line_levels);
    free(c->pieces);
    free(c->order);
    memset(c, 0, sizeof *c);
}

void ui_paragraph_free(ui_paragraph *p) {
    free(p->text);
    free(p->spans);
    free(p->families);
    free(p->family_offsets);
    free(p->ellipsis);
    ui_shaped_paragraph_free(&p->shaped);
    ui_paragraph_layout_free(&p->layout);
    free(p->clusters);
    free(p->line_clusters);
    memset(p, 0, sizeof *p);
}

static void *dup_bytes(const void *src, size_t n) {
    void *d = malloc(n ? n : 1);
    if (d && n) memcpy(d, src, n);
    return d;
}

ui_result ui_paragraph_set(ui_paragraph *p, const ui_paragraph_desc *d) {
    uint8_t *text = dup_bytes(d->text, d->len);
    ui_text_span *spans = dup_bytes(d->spans, d->span_count * sizeof *spans);
    uint32_t fam_bytes = d->family_count ? d->family_offsets[d->family_count] : 0;
    char *families = dup_bytes(d->families, fam_bytes);
    uint32_t *offsets = dup_bytes(d->family_offsets,
                                  d->family_count ? (d->family_count + 1) * sizeof *offsets : 0);
    uint8_t *ellipsis = dup_bytes(d->ellipsis, d->ellipsis_len);
    if (!text || !spans || !families || !offsets || !ellipsis) {
        free(text);
        free(spans);
        free(families);
        free(offsets);
        free(ellipsis);
        return UI_ERR_OUT_OF_MEMORY;
    }
    free(p->text);
    free(p->spans);
    free(p->families);
    free(p->family_offsets);
    free(p->ellipsis);
    p->text = text;
    p->len = d->len;
    p->base = d->base;
    p->spans = spans;
    p->span_count = d->span_count;
    p->families = families;
    p->family_offsets = offsets;
    p->family_count = d->family_count;
    p->dir = d->dir;
    p->max_lines = d->max_lines;
    p->ellipsis = ellipsis;
    p->ellipsis_len = d->ellipsis_len;
    p->align = d->align;
    p->line_height = d->line_height > 0 && isfinite(d->line_height) ? d->line_height : 0;
    p->shaped_valid = false;
    p->layout_valid = false;
    p->hits_valid = false;
    return UI_OK;
}

static ui_text_input input_of(const ui_paragraph *p) {
    return (ui_text_input){p->text, p->len, p->base, p->spans, p->span_count, p->families,
                           p->family_offsets, p->family_count, p->dir};
}

// ---- Line breaking -----------------------------------------------------------------------

#define TAB_SPACES 8.f

// A tab advances to the next stop: every TAB_SPACES space widths from the line start.
// `space` is the tab's own advance from shaping (its font's space width).
static float tab_advance(float pen, float space) {
    if (!(space > 0)) return 0;
    float interval = space * TAB_SPACES;
    return (floorf((pen + EPS) / interval) + 1) * interval - pen;
}

// Whitespace that hangs at the end of a line instead of counting toward its width.
static bool hangs(uint32_t cp) {
    if (cp == '\t') return true; // preserved tabs hang like spaces (CSS pre-wrap)
    uint8_t lb = ui_ucd(cp)->lb;
    return lb == UI_LB_SP || lb == UI_LB_BK || lb == UI_LB_CR || lb == UI_LB_LF ||
           lb == UI_LB_NL || lb == UI_LB_ZW;
}

// The end (exclusive code point index) of the line starting at ls: a mandatory break, the
// last break opportunity that fits, or an emergency break at a grapheme boundary when not
// even one word fits. Trailing whitespace hangs and never causes a break.
static uint32_t break_line(const ui_shaped_paragraph *p, const float *adv, uint32_t ls,
                           float max_w) {
    uint32_t n = p->cp_count;
    float w = 0, trailing = 0;
    uint32_t last_break = 0;
    bool have_break = false;
    for (uint32_t i = ls; i < n; i++) {
        if (i > ls) {
            uint8_t b = p->line_breaks[i];
            if (b == UI_LINE_BREAK_MANDATORY) return i;
            if (b == UI_LINE_BREAK_ALLOWED) {
                last_break = i;
                have_break = true;
            }
        }
        float a = p->cps[i] == '\t' ? tab_advance(w, adv[i]) : adv[i];
        w += a;
        trailing = hangs(p->cps[i]) ? trailing + a : 0;
        if (w - trailing > max_w + EPS) {
            if (have_break) return last_break;
            uint32_t g = i;
            while (g > ls && !p->graphemes[g]) g--;
            if (g > ls) return g;
            g = ls + 1;
            while (g < n && !p->graphemes[g]) g++;
            return g;
        }
    }
    return n;
}

// ---- Building lines ----------------------------------------------------------------------

typedef struct builder {
    ui_paragraph *para;
    ui_text_context *ctx;
    ui_text_input in;
    ui_paragraph_layout *out;
    ui_piece *pieces;
    uint32_t piece_count;
    // Fixed line boxes (line_height > 0): height, and the base font's ascent and descent.
    bool fixed;
    float fixed_height, fixed_ascent, fixed_descent;
} builder;

static ui_result push_glyphs(builder *b, const ui_glyph_pos *g, uint32_t count, bool reverse,
                             uint32_t cluster_override, uint32_t style_override, uint32_t *start) {
    ui_paragraph_layout *o = b->out;
    if (ui_grow((void **)&o->glyphs, &o->glyph_capacity, o->glyph_count + count + 1,
                sizeof *o->glyphs) != UI_OK)
        return UI_ERR_OUT_OF_MEMORY;
    const ui_shaped_paragraph *sp = &b->para->shaped;
    *start = o->glyph_count;
    for (uint32_t i = 0; i < count; i++) {
        const ui_glyph_pos *s = &g[reverse ? count - 1 - i : i];
        uint32_t cluster = cluster_override != UINT32_MAX ? cluster_override : s->cluster;
        uint32_t style = style_override != UINT32_MAX ? style_override
                                                      : sp->styles[ui_shaped_cp_at(sp, cluster)];
        // x and y hold the HarfBuzz offsets until the line is positioned.
        o->glyphs[o->glyph_count++] = (ui_layout_glyph){
            .glyph = s->glyph, .cluster = cluster,
            .x = (float)s->x_offset / 64.f, .y = (float)s->y_offset / 64.f,
            .advance = (float)s->advance / 64.f, .style = style,
        };
    }
    return UI_OK;
}

static bool boundary_safe(const ui_shaped_run *run, const ui_glyph_pos *g, uint32_t byte) {
    if (byte == run->start || byte == run->end) return true;
    for (uint32_t i = 0; i < run->glyph_count; i++) {
        if (g[i].cluster == byte) return !g[i].unsafe_to_break;
    }
    return false; // inside a cluster (a ligature split by an emergency break)
}

static ui_result add_piece(builder *b, uint32_t r, uint32_t start, uint32_t end, uint8_t level) {
    ui_text_context *c = b->ctx;
    if (ui_grow((void **)&c->pieces, &c->piece_capacity, b->piece_count + 1, sizeof *c->pieces) != UI_OK)
        return UI_ERR_OUT_OF_MEMORY;
    b->pieces = c->pieces;
    const ui_shaped_paragraph *sp = &b->para->shaped;
    const ui_shaped_run *run = &sp->runs[r];
    const ui_glyph_pos *g = sp->glyphs + run->glyph_start;
    bool reverse = (level & 1) != (run->level & 1); // L1 changed the direction (whitespace)
    ui_piece *pc = &b->pieces[b->piece_count++];
    *pc = (ui_piece){start, end, r, level, false, 0, 0};
    ui_result res;
    if (start == run->start && end == run->end) {
        res = push_glyphs(b, g, run->glyph_count, reverse, UINT32_MAX, UINT32_MAX, &pc->glyph_start);
        pc->glyph_count = run->glyph_count;
    } else if (boundary_safe(run, g, start) && boundary_safe(run, g, end)) {
        // Glyphs of [start, end) are contiguous in either visual order.
        uint32_t first = UINT32_MAX, count = 0;
        for (uint32_t i = 0; i < run->glyph_count; i++) {
            if (g[i].cluster >= start && g[i].cluster < end) {
                if (first == UINT32_MAX) first = i;
                count++;
            }
        }
        res = push_glyphs(b, g + (first == UINT32_MAX ? 0 : first), count, reverse, UINT32_MAX,
                          UINT32_MAX, &pc->glyph_start);
        pc->glyph_count = count;
    } else {
        const ui_glyph_pos *rg;
        uint32_t count;
        res = ui_reshape_range(&c->shaper, c->fonts, &b->in, run, start, end, &rg, &count);
        if (res == UI_OK)
            res = push_glyphs(b, rg, count, reverse, UINT32_MAX, UINT32_MAX, &pc->glyph_start);
        pc->glyph_count = count;
    }
    return res;
}

// Shapes the ellipsis in the font of run r. Falls back to "..." when that font lacks a glyph
// of the default ellipsis.
static ui_result add_ellipsis(builder *b, uint32_t r, uint32_t at, uint8_t para_level) {
    ui_paragraph *p = b->para;
    ui_text_context *c = b->ctx;
    const ui_shaped_run *run = &p->shaped.runs[r];
    ui_font *font = ui_font_registry_font_by_key(c->fonts, run->font);
    if (!font) return UI_ERR_OUT_OF_MEMORY;
    const uint8_t *text = p->ellipsis;
    uint32_t len = p->ellipsis_len;
    const ui_glyph_pos *g;
    uint32_t count;
    for (int attempt = 0; attempt < 2; attempt++) {
        ui_shape_request req = {text, len, 0, len, run->script, para_level & 1};
        ui_result res = ui_shape(c->shaper.hb, font, &req, &g, &count);
        if (res != UI_OK) return res;
        bool missing = false;
        for (uint32_t i = 0; i < count; i++) missing |= g[i].glyph == 0;
        if (!missing || attempt) break;
        text = (const uint8_t *)"...";
        len = 3;
    }
    if (ui_grow((void **)&c->pieces, &c->piece_capacity, b->piece_count + 1, sizeof *c->pieces) != UI_OK)
        return UI_ERR_OUT_OF_MEMORY;
    b->pieces = c->pieces;
    ui_piece *pc = &b->pieces[b->piece_count++];
    *pc = (ui_piece){at, at, r, para_level, true, 0, count};
    uint32_t cp = at > 0 ? ui_shaped_cp_at(&p->shaped, at - 1) : 0;
    uint32_t style = p->shaped.cp_count ? p->shaped.styles[cp] : 0;
    return push_glyphs(b, g, count, false, at, style, &pc->glyph_start);
}

static float ellipsis_width(builder *b, uint32_t r, uint8_t para_level) {
    // Shape once to measure; the pieces are discarded by the caller resetting counts.
    uint32_t pieces = b->piece_count, glyphs = b->out->glyph_count;
    if (add_ellipsis(b, r, 0, para_level) != UI_OK) return 0;
    float w = 0;
    for (uint32_t i = glyphs; i < b->out->glyph_count; i++) w += b->out->glyphs[i].advance;
    b->piece_count = pieces;
    b->out->glyph_count = glyphs;
    return w;
}

static uint32_t run_at(const ui_shaped_paragraph *p, uint32_t byte, uint32_t hint) {
    uint32_t r = hint < p->run_count ? hint : 0;
    if (p->runs[r].start > byte) r = 0;
    while (r + 1 < p->run_count && p->runs[r].end <= byte) r++;
    return r;
}

// Lays out code points [lcs, lce) as one line; with `ellipsis_run` != NO_RUN the ellipsis
// (in that run's font) follows the content.
static ui_result build_line(builder *b, uint32_t lcs, uint32_t lce, uint32_t metrics_run,
                            uint32_t ellipsis_run, float top, float *height_out) {
    ui_paragraph *p = b->para;
    ui_text_context *c = b->ctx;
    const ui_shaped_paragraph *sp = &p->shaped;
    ui_paragraph_layout *o = b->out;
    uint32_t lb = sp->offsets[lcs], le = sp->offsets[lce];
    uint8_t para_level = sp->cp_count ? sp->bidi.para_level[lcs < sp->cp_count ? lcs : lcs - 1] : 0;
    b->piece_count = 0;
    if (ui_grow((void **)&c->line_levels, &c->level_capacity, lce - lcs + 1, 1) != UI_OK)
        return UI_ERR_OUT_OF_MEMORY;
    if (lce > lcs) ui_bidi_line_levels(&sp->bidi, lcs, lce, c->line_levels);
    uint32_t r = lce > lcs ? run_at(sp, lb, 0) : 0;
    for (; lce > lcs && r < sp->run_count && sp->runs[r].start < le; r++) {
        uint32_t rs = sp->runs[r].start > lb ? sp->runs[r].start : lb;
        uint32_t re = sp->runs[r].end < le ? sp->runs[r].end : le;
        uint32_t cp = ui_shaped_cp_at(sp, rs);
        uint32_t a = rs;
        while (a < re) {
            uint8_t level = c->line_levels[cp - lcs];
            uint32_t c2 = cp + 1;
            while (c2 < lce && sp->offsets[c2] < re && c->line_levels[c2 - lcs] == level) c2++;
            uint32_t bb = c2 < lce && sp->offsets[c2] < re ? sp->offsets[c2] : re;
            ui_result res = add_piece(b, r, a, bb, level);
            if (res != UI_OK) return res;
            a = bb;
            cp = c2;
        }
    }
    if (ellipsis_run != NO_RUN) {
        ui_result res = add_ellipsis(b, ellipsis_run, le, para_level);
        if (res != UI_OK) return res;
    }

    // L2 over pieces (each has one level), then position in visual order.
    if (ui_grow((void **)&c->order, &c->order_capacity, b->piece_count + 1, sizeof *c->order) != UI_OK ||
        ui_grow((void **)&c->line_levels, &c->level_capacity, b->piece_count + lce - lcs + 1, 1) != UI_OK)
        return UI_ERR_OUT_OF_MEMORY;
    uint8_t *piece_levels = c->line_levels + (lce - lcs);
    for (uint32_t i = 0; i < b->piece_count; i++) piece_levels[i] = b->pieces[i].level;
    ui_bidi_reorder(piece_levels, b->piece_count, c->order);

    if (ui_grow((void **)&o->runs, &o->run_capacity, o->run_count + b->piece_count + 1,
                sizeof *o->runs) != UI_OK ||
        ui_grow((void **)&o->lines, &o->line_capacity, o->line_count + 1, sizeof *o->lines) != UI_OK)
        return UI_ERR_OUT_OF_MEMORY;
    float asc = 0, desc = 0, gap = 0;
    if (!b->piece_count && metrics_run < sp->run_count) {
        asc = sp->runs[metrics_run].ascent;
        desc = sp->runs[metrics_run].descent;
        gap = sp->runs[metrics_run].line_gap;
    }
    float pen = 0;
    uint32_t run_start = o->run_count;
    for (uint32_t k = 0; k < b->piece_count; k++) {
        const ui_piece *pc = &b->pieces[c->order[k]];
        const ui_shaped_run *srun = &sp->runs[pc->run];
        asc = fmaxf(asc, srun->ascent);
        desc = fmaxf(desc, srun->descent);
        gap = fmaxf(gap, srun->line_gap);
        ui_layout_run *lr = &o->runs[o->run_count++];
        *lr = (ui_layout_run){.font = srun->font, .start = pc->start, .end = pc->end,
                              .level = pc->level, .ellipsis = pc->ellipsis,
                              .x = pen, .glyph_start = pc->glyph_start,
                              .glyph_count = pc->glyph_count};
        for (uint32_t i = 0; i < pc->glyph_count; i++) {
            ui_layout_glyph *g = &o->glyphs[pc->glyph_start + i];
            g->x += pen; // still relative to the line start; y still the offset
            if (g->glyph == UI_GLYPH_INVISIBLE && g->advance > 0) // a tab: to the next stop
                g->advance = tab_advance(pen, g->advance);
            pen += g->advance;
        }
        lr->width = pen - lr->x;
    }
    // Trailing whitespace (logical end) hangs: it is laid out but not counted.
    // Measured on the positioned glyphs: a tab's width depends on where it sits.
    float trailing = 0;
    if (ellipsis_run == NO_RUN) {
        uint32_t ts = lce;
        while (ts > lcs && hangs(sp->cps[ts - 1])) ts--;
        uint32_t from = sp->offsets[ts];
        for (uint32_t k = run_start; k < o->run_count; k++) {
            const ui_layout_run *lr = &o->runs[k];
            for (uint32_t i = 0; i < lr->glyph_count; i++) {
                const ui_layout_glyph *g = &o->glyphs[lr->glyph_start + i];
                if (g->cluster >= from && g->cluster < le) trailing += g->advance;
            }
        }
    }
    float visible = fmaxf(pen - trailing, 0);
    float height = roundf(asc + desc + gap);
    float baseline = roundf(top + gap * 0.5f + asc);
    if (b->fixed) {
        // Half-leading around the base font: the same box and baseline on every line.
        asc = b->fixed_ascent;
        desc = b->fixed_descent;
        height = b->fixed_height;
        baseline = roundf(top + (height - asc - desc) * 0.5f + asc);
    }
    o->lines[o->line_count++] = (ui_line){
        .start = lb, .end = le, .top = top, .height = height, .ascent = asc, .descent = desc,
        .baseline = baseline, .x = (para_level & 1) ? -trailing : 0, .width = visible,
        .run_start = run_start, .run_count = o->run_count - run_start,
        .ellipsized = ellipsis_run != NO_RUN,
    };
    *height_out = height;
    return UI_OK;
}

// The longest prefix of [ls, le) that fits with an ellipsis of width e_w, ending at a
// grapheme boundary, with trailing whitespace and line breaks dropped.
static uint32_t ellipsis_cut(const ui_shaped_paragraph *p, const float *adv, uint32_t ls,
                             uint32_t le, float max_w, float e_w) {
    float w = 0;
    uint32_t best = ls;
    for (uint32_t i = ls; i < le; i++) {
        w += p->cps[i] == '\t' ? tab_advance(w, adv[i]) : adv[i];
        uint32_t next = i + 1;
        if (next < p->cp_count && next < le && !p->graphemes[next]) continue;
        if (hangs(p->cps[i])) continue; // never end the content on whitespace
        if (w + e_w > max_w + EPS) break;
        best = next;
    }
    return best;
}

// Justification: stretches the spaces of a line (not its hanging trailing whitespace) so its
// visible content fills `avail`. The last line of a paragraph, an ellipsized line and a line
// without spaces are not justified (false). Glyph positions are still relative to the line.
static bool justify_line(const ui_paragraph *p, ui_paragraph_layout *o, ui_line *ln, float avail) {
    const ui_shaped_paragraph *sp = &p->shaped;
    float extra = avail - ln->width;
    if (ln->ellipsized || ln->end <= ln->start || ln->end >= p->len || !(extra > EPS)) return false;
    uint32_t last = ui_shaped_cp_at(sp, ln->end - 1);
    uint8_t lb = ui_ucd(sp->cps[last])->lb;
    if (lb == UI_LB_BK || lb == UI_LB_CR || lb == UI_LB_LF || lb == UI_LB_NL) return false;
    uint32_t ts = ui_shaped_cp_at(sp, ln->end); // trailing whitespace starts at offsets[ts]
    while (ts > 0 && sp->offsets[ts - 1] >= ln->start && hangs(sp->cps[ts - 1])) ts--;
    uint32_t visible_end = sp->offsets[ts];
    uint32_t spaces = 0;
    for (uint32_t k = 0; k < ln->run_count; k++) {
        const ui_layout_run *lr = &o->runs[ln->run_start + k];
        for (uint32_t g = 0; g < lr->glyph_count; g++) {
            const ui_layout_glyph *gl = &o->glyphs[lr->glyph_start + g];
            if (!lr->ellipsis && gl->cluster < visible_end &&
                ui_ucd(sp->cps[ui_shaped_cp_at(sp, gl->cluster)])->gc == UI_GC_ZS)
                spaces++;
        }
    }
    if (!spaces) return false;
    float per = extra / (float)spaces, shift = 0;
    for (uint32_t k = 0; k < ln->run_count; k++) {
        ui_layout_run *lr = &o->runs[ln->run_start + k];
        lr->x += shift;
        float grown = 0;
        for (uint32_t g = 0; g < lr->glyph_count; g++) {
            ui_layout_glyph *gl = &o->glyphs[lr->glyph_start + g];
            gl->x += shift;
            if (!lr->ellipsis && gl->cluster < visible_end &&
                ui_ucd(sp->cps[ui_shaped_cp_at(sp, gl->cluster)])->gc == UI_GC_ZS) {
                gl->advance += per;
                shift += per;
                grown += per;
            }
        }
        lr->width += grown;
    }
    ln->width = avail;
    return true;
}

static ui_result layout(ui_paragraph *p, ui_text_context *c, float max_w) {
    ui_shaped_paragraph *sp = &p->shaped;
    ui_paragraph_layout *o = &p->layout;
    o->line_count = o->run_count = o->glyph_count = 0;
    o->max_width = max_w;
    o->truncated = false;
    uint32_t n = sp->cp_count;
    if (ui_grow((void **)&c->advances, &c->advance_capacity, n + 1, sizeof *c->advances) != UI_OK)
        return UI_ERR_OUT_OF_MEMORY;
    memset(c->advances, 0, (n + 1) * sizeof *c->advances);
    for (uint32_t r = 0; r < sp->run_count; r++) {
        const ui_glyph_pos *g = sp->glyphs + sp->runs[r].glyph_start;
        for (uint32_t i = 0; i < sp->runs[r].glyph_count; i++)
            c->advances[ui_shaped_cp_at(sp, g[i].cluster)] += (float)g[i].advance / 64.f;
    }
    o->para_level = n ? sp->bidi.para_level[0] : (p->dir == UI_BIDI_RTL);

    builder b = {p, c, input_of(p), o, c->pieces, 0, false, 0, 0, 0};
    float top = 0;
    // The base style's font: metrics for empty text and for fixed line boxes.
    ui_font_metrics base = {0};
    if (n == 0 || p->line_height > 0) {
        ui_text_style *st = &p->base;
        const char *fam;
        uint32_t fam_len;
        ui_text_input in = input_of(p);
        ui_text_input_family(&in, st, &fam, &fam_len);
        uint32_t face = ui_font_registry_match(c->fonts, fam, fam_len, st->weight,
                                               (ui_font_style)st->style);
        if (face == UI_NO_FACE) return UI_ERR_WRONG_STATE;
        ui_font *font = ui_font_registry_font(c->fonts, face, st->size * sp->scale, st->weight,
                                              (ui_font_style)st->style, NULL);
        if (!font) return UI_ERR_OUT_OF_MEMORY;
        ui_font_get_metrics(font, &base);
    }
    if (p->line_height > 0) {
        b.fixed = true;
        b.fixed_height = fmaxf(roundf(p->line_height * p->base.size * sp->scale), 1);
        b.fixed_ascent = base.ascent;
        b.fixed_descent = base.descent;
    }
    if (n == 0) {
        // One empty line in the base font, so carets have somewhere to go.
        if (ui_grow((void **)&o->lines, &o->line_capacity, 1, sizeof *o->lines) != UI_OK)
            return UI_ERR_OUT_OF_MEMORY;
        float h = b.fixed ? b.fixed_height : roundf(base.ascent + base.descent + base.line_gap);
        float lead = b.fixed ? (h - base.ascent - base.descent) * 0.5f : base.line_gap * 0.5f;
        o->lines[o->line_count++] = (ui_line){.height = h, .ascent = base.ascent,
                                              .descent = base.descent,
                                              .baseline = roundf(lead + base.ascent)};
        top = h;
    }
    uint32_t ls = 0, lines = 0;
    while (ls < n) {
        uint32_t le = break_line(sp, c->advances, ls, max_w);
        lines++;
        float h;
        ui_result res;
        if (p->max_lines && lines == p->max_lines && le < n) {
            o->truncated = true;
            uint32_t r = run_at(sp, sp->offsets[le > ls ? le - 1 : ls], 0);
            uint32_t cut = le;
            if (p->ellipsis_len) {
                float e_w = ellipsis_width(&b, r, sp->bidi.para_level[ls]);
                cut = ellipsis_cut(sp, c->advances, ls, le, max_w, e_w);
                if (cut > ls) r = run_at(sp, sp->offsets[cut - 1], 0);
            } else {
                while (cut > ls && hangs(sp->cps[cut - 1])) cut--;
            }
            res = build_line(&b, ls, cut, r, p->ellipsis_len ? r : NO_RUN, top, &h);
            if (res != UI_OK) return res;
            top += h;
            break;
        }
        res = build_line(&b, ls, le, NO_RUN, NO_RUN, top, &h);
        if (res != UI_OK) return res;
        top += h;
        ls = le;
        // Text ending in a hard break gets an empty last line (where a caret goes next).
        uint8_t lb = ui_ucd(sp->cps[le - 1])->lb;
        if (le == n && (lb == UI_LB_BK || lb == UI_LB_CR || lb == UI_LB_LF || lb == UI_LB_NL) &&
            !(p->max_lines && lines == p->max_lines)) {
            res = build_line(&b, n, n, sp->run_count - 1, NO_RUN, top, &h);
            if (res != UI_OK) return res;
            top += h;
        }
    }

    // Alignment. Unbounded layouts align to the widest line.
    float width = 0;
    for (uint32_t i = 0; i < o->line_count; i++) width = fmaxf(width, o->lines[i].width);
    float avail = isfinite(max_w) ? max_w : width;
    for (uint32_t i = 0; i < o->line_count; i++) {
        ui_line *ln = &o->lines[i];
        uint8_t level = ln->end > ln->start && n
                            ? sp->bidi.para_level[ui_shaped_cp_at(sp, ln->start)]
                            : o->para_level;
        bool rtl = level & 1;
        ui_align align = p->align;
        if (align == UI_ALIGN_JUSTIFY && !justify_line(p, o, ln, avail)) align = UI_ALIGN_START;
        if (align == UI_ALIGN_START) align = rtl ? UI_ALIGN_RIGHT : UI_ALIGN_LEFT;
        else if (align == UI_ALIGN_END) align = rtl ? UI_ALIGN_LEFT : UI_ALIGN_RIGHT;
        float x = align == UI_ALIGN_RIGHT    ? avail - ln->width
                  : align == UI_ALIGN_CENTER ? (avail - ln->width) * 0.5f
                                             : 0;
        // ln->x holds the pen origin relative to the visible start: -trailing in RTL lines,
        // whose hanging whitespace sits on the left.
        float origin = x + ln->x;
        ln->x = x;
        for (uint32_t k = 0; k < ln->run_count; k++) {
            ui_layout_run *lr = &o->runs[ln->run_start + k];
            lr->x += origin;
            for (uint32_t g = 0; g < lr->glyph_count; g++) {
                ui_layout_glyph *gl = &o->glyphs[lr->glyph_start + g];
                gl->x += origin;
                gl->y = ln->baseline - gl->y;
            }
        }
    }
    o->width = width;
    o->height = top;
    return UI_OK;
}

const ui_paragraph_layout *ui_paragraph_layout_get(ui_paragraph *p, ui_text_context *c,
                                                   float scale, float max_width,
                                                   ui_result *result) {
    if (!(max_width >= 0)) max_width = INFINITY;
    *result = UI_OK;
    if (!p->shaped_valid || p->shaped.scale != scale) {
        ui_text_input in = input_of(p);
        *result = ui_shape_paragraph(&c->shaper, c->fonts, &in, scale, &p->shaped);
        if (*result != UI_OK) return NULL;
        p->shaped_valid = true;
        p->layout_valid = false;
        p->shape_count++;
    }
    if (!p->layout_valid || p->layout.max_width != max_width) {
        *result = layout(p, c, max_width);
        if (*result != UI_OK) {
            p->layout_valid = false;
            return NULL;
        }
        p->layout_valid = true;
        p->hits_valid = false;
        p->layout_count++;
    }
    return &p->layout;
}
