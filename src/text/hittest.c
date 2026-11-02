#include "text/hittest.h"

#include <math.h>
#include <stdlib.h>

#include "unicode/ucd.h"

#define MAX_LIGATURE_PARTS 16

static bool hard_break(uint32_t cp) {
    uint8_t lb = ui_ucd(cp)->lb;
    return lb == UI_LB_BK || lb == UI_LB_CR || lb == UI_LB_LF || lb == UI_LB_NL;
}

static ui_result push(ui_paragraph *p, ui_hit_cluster c) {
    if (ui_grow((void **)&p->clusters, &p->cluster_capacity, p->cluster_count + 1,
                sizeof *p->clusters) != UI_OK)
        return UI_ERR_OUT_OF_MEMORY;
    p->clusters[p->cluster_count++] = c;
    return UI_OK;
}

// Adds the clusters of one glyph group [start, end) spanning x0..x1, split per grapheme.
static ui_result add_group(ui_paragraph *p, ui_text_context *c, const ui_layout_run *run,
                           const ui_layout_glyph *first, uint32_t glyphs, uint32_t start,
                           uint32_t end, float x0, float x1, uint32_t line) {
    const ui_shaped_paragraph *sp = &p->shaped;
    bool rtl = run->level & 1;
    uint32_t bounds[MAX_LIGATURE_PARTS + 1];
    uint32_t parts = 0;
    for (uint32_t cp = ui_shaped_cp_at(sp, start); cp < sp->cp_count && sp->offsets[cp] < end; cp++) {
        if (sp->graphemes[cp] && parts < MAX_LIGATURE_PARTS) bounds[parts++] = sp->offsets[cp];
    }
    if (!parts) bounds[parts++] = start;
    bounds[parts] = end;
    // A lone hard break is no caret target: the caret goes before it, never after it.
    if (parts == 1 && hard_break(sp->cps[ui_shaped_cp_at(sp, start)])) return UI_OK;
    float carets[MAX_LIGATURE_PARTS];
    bool have_carets = false;
    if (parts > 1 && glyphs == 1) {
        ui_font *font = ui_font_registry_font_by_key(c->fonts, run->font);
        have_carets = font && ui_font_ligature_carets(font, first->glyph, rtl, carets,
                                                      parts - 1) == parts - 1;
    }
    float w = x1 - x0;
    for (uint32_t k = 0; k < parts; k++) {
        // Logical part k spans [a, b) of the width measured from the logical start edge.
        float a = k == 0 ? 0 : have_carets ? carets[k - 1] : w * (float)k / (float)parts;
        float b = k + 1 == parts ? w : have_carets ? carets[k] : w * (float)(k + 1) / (float)parts;
        ui_hit_cluster hc = {bounds[k], bounds[k + 1], 0, 0, line, rtl};
        hc.x0 = rtl ? x1 - b : x0 + a;
        hc.x1 = rtl ? x1 - a : x0 + b;
        ui_result res = push(p, hc);
        if (res != UI_OK) return res;
    }
    // RTL parts were pushed in logical order; reverse them into visual order.
    if (rtl) {
        ui_hit_cluster *v = p->clusters + p->cluster_count - parts;
        for (uint32_t i = 0, j = parts - 1; i < j; i++, j--) {
            ui_hit_cluster t = v[i];
            v[i] = v[j];
            v[j] = t;
        }
    }
    return UI_OK;
}

ui_result ui_text_hit_prepare(ui_paragraph *p, ui_text_context *c) {
    if (!p->layout_valid) return UI_ERR_WRONG_STATE;
    if (p->hits_valid) return UI_OK;
    const ui_paragraph_layout *l = &p->layout;
    p->cluster_count = 0;
    if (ui_grow((void **)&p->line_clusters, &p->line_cluster_capacity, l->line_count + 1,
                sizeof *p->line_clusters) != UI_OK)
        return UI_ERR_OUT_OF_MEMORY;
    for (uint32_t li = 0; li < l->line_count; li++) {
        const ui_line *ln = &l->lines[li];
        p->line_clusters[li] = p->cluster_count;
        for (uint32_t r = 0; r < ln->run_count; r++) {
            const ui_layout_run *run = &l->runs[ln->run_start + r];
            if (run->ellipsis) continue;
            const ui_layout_glyph *g = l->glyphs + run->glyph_start;
            bool rtl = run->level & 1;
            // Consecutive glyphs with one cluster value form a group (a grapheme, or a
            // ligature of several). Its logical end is the next cluster in logical order:
            // the group to the right for LTR, to the left for RTL.
            for (uint32_t i = 0; i < run->glyph_count;) {
                uint32_t j = i + 1;
                while (j < run->glyph_count && g[j].cluster == g[i].cluster) j++;
                uint32_t end;
                if (!rtl) end = j < run->glyph_count ? g[j].cluster : run->end;
                else end = i > 0 ? g[i - 1].cluster : run->end;
                // The group spans its glyphs' advances from the first glyph's position (a
                // cluster's first glyph is its base, which carries no offset in practice).
                float x0 = g[i].x, x1 = x0;
                for (uint32_t k = i; k < j; k++) x1 += g[k].advance;
                ui_result res = add_group(p, c, run, &g[i], j - i, g[i].cluster, end, x0, x1, li);
                if (res != UI_OK) return res;
                i = j;
            }
        }
    }
    p->line_clusters[l->line_count] = p->cluster_count;
    p->hits_valid = true;
    return UI_OK;
}

static uint32_t line_at_y(const ui_paragraph_layout *l, float y) {
    for (uint32_t i = 0; i < l->line_count; i++) {
        if (y < l->lines[i].top + l->lines[i].height) return i;
    }
    return l->line_count ? l->line_count - 1 : 0;
}

ui_result ui_text_hit_point(ui_paragraph *p, ui_text_context *c, float x, float y,
                            ui_text_position *out) {
    ui_result res = ui_text_hit_prepare(p, c);
    if (res != UI_OK) return res;
    const ui_paragraph_layout *l = &p->layout;
    uint32_t li = line_at_y(l, y);
    const ui_line *ln = &l->lines[li];
    uint32_t first = p->line_clusters[li], last = p->line_clusters[li + 1];
    if (first == last) {
        // An empty line (or one holding only a hard break): its start.
        *out = (ui_text_position){ln->start, UI_AFFINITY_DOWNSTREAM};
        return UI_OK;
    }
    const ui_hit_cluster *hit = &p->clusters[first];
    bool leading;
    if (x < p->clusters[first].x0) {
        leading = !hit->rtl; // left edge: leading side of an LTR cluster
    } else if (x >= p->clusters[last - 1].x1) {
        hit = &p->clusters[last - 1];
        leading = hit->rtl;
    } else {
        for (uint32_t i = first; i < last; i++) {
            if (x < p->clusters[i].x1) {
                hit = &p->clusters[i];
                break;
            }
        }
        float mid = (hit->x0 + hit->x1) * 0.5f;
        leading = hit->rtl ? x >= mid : x < mid;
    }
    *out = leading ? (ui_text_position){hit->start, UI_AFFINITY_DOWNSTREAM}
                   : (ui_text_position){hit->end, UI_AFFINITY_UPSTREAM};
    return UI_OK;
}

// The line a position belongs to: at a soft wrap the offset ends one line and starts the
// next, and affinity picks; after a hard break it is always the next line.
static uint32_t line_of(const ui_paragraph *p, ui_text_position pos) {
    const ui_paragraph_layout *l = &p->layout;
    for (uint32_t i = 0; i < l->line_count; i++) {
        const ui_line *ln = &l->lines[i];
        if (pos.offset < ln->start) continue;
        if (pos.offset < ln->end) return i;
        if (pos.offset == ln->end) {
            bool next_starts_here = i + 1 < l->line_count && l->lines[i + 1].start == pos.offset;
            if (!next_starts_here) return i;
            uint32_t last_cp = ln->end > ln->start ? ui_shaped_cp_at(&p->shaped, ln->end - 1) : 0;
            bool hard = ln->end > ln->start && hard_break(p->shaped.cps[last_cp]);
            if (!hard && pos.affinity == UI_AFFINITY_UPSTREAM) return i;
        }
    }
    return l->line_count ? l->line_count - 1 : 0;
}

ui_result ui_text_caret_rect(ui_paragraph *p, ui_text_context *c, ui_text_position pos,
                             float caret_width, ui_text_rect *out) {
    ui_result res = ui_text_hit_prepare(p, c);
    if (res != UI_OK) return res;
    const ui_paragraph_layout *l = &p->layout;
    uint32_t li = line_of(p, pos);
    const ui_line *ln = &l->lines[li];
    uint32_t first = p->line_clusters[li], last = p->line_clusters[li + 1];
    const ui_hit_cluster *before = NULL, *after = NULL, *inside = NULL;
    for (uint32_t i = first; i < last; i++) {
        const ui_hit_cluster *h = &p->clusters[i];
        if (h->start == pos.offset) after = h;
        if (h->end == pos.offset) before = h;
        if (h->start < pos.offset && pos.offset < h->end) inside = h;
    }
    float x;
    if (inside) { // inside a cluster: snap to its start
        after = inside;
        before = NULL;
    }
    if (after && (pos.affinity == UI_AFFINITY_DOWNSTREAM || !before))
        x = after->rtl ? after->x1 : after->x0; // leading edge of the following character
    else if (before)
        x = before->rtl ? before->x0 : before->x1; // trailing edge of the preceding one
    else
        x = (l->para_level & 1) ? ln->x + ln->width : ln->x; // an empty line
    *out = (ui_text_rect){x - caret_width * 0.5f, ln->top, caret_width, ln->height};
    return UI_OK;
}

ui_result ui_text_selection_rects(ui_paragraph *p, ui_text_context *c, uint32_t start,
                                  uint32_t end, ui_text_rect *rects, uint32_t cap,
                                  uint32_t *count) {
    ui_result res = ui_text_hit_prepare(p, c);
    if (res != UI_OK) return res;
    const ui_paragraph_layout *l = &p->layout;
    uint32_t n = 0;
    for (uint32_t li = 0; li < l->line_count && start < end; li++) {
        const ui_line *ln = &l->lines[li];
        bool open = false;
        ui_text_rect cur = {0};
        for (uint32_t i = p->line_clusters[li]; i < p->line_clusters[li + 1]; i++) {
            const ui_hit_cluster *h = &p->clusters[i];
            bool in = h->start < end && h->end > start;
            if (in && open && fabsf(h->x0 - (cur.x + cur.width)) < 0.01f) {
                cur.width = h->x1 - cur.x;
                continue;
            }
            if (open) {
                if (n < cap) rects[n] = cur;
                n++;
                open = false;
            }
            if (in) {
                cur = (ui_text_rect){h->x0, ln->top, h->x1 - h->x0, ln->height};
                open = true;
            }
        }
        if (open) {
            if (n < cap) rects[n] = cur;
            n++;
        }
    }
    *count = n;
    return UI_OK;
}
