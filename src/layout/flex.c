// The flex layout algorithm (CSS Flexible Box Layout Level 1, §9), with the intrinsic sizes
// of flex containers computed as Chrome does. Steps refer to the specification's numbering.
//
// Every computation takes what the parent knows about the node: sizes it fixed (`known`,
// NaN when not), the parent's content box for percentages (`parent`), and the space
// available (a definite size, or a min-content or max-content constraint). It returns the
// node's border-box size and first baseline, and with `perform` also positions its children.
// Results are cached per node by those inputs (layout.h).
#include "layout/layout.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "core/scene.h"

// What a computation does: measure the node, also position its children, or measure its
// content in one axis, ignoring its own size, min and max in that axis (intrinsic sizes and
// the content-based sizes flex items need: §9.2.3, §4.5). Constraints in the other axis
// still apply: a max-width decides where text wraps, and so its height.
// A plain measurement leaves the baseline out unless it comes for free: only a parent that
// aligns the node by baseline asks for it (MEASURE_BASELINE), which may cost measuring the
// node's first item.
typedef enum run { MEASURE, MEASURE_BASELINE, PERFORM, INTRINSIC_X, INTRINSIC_Y } run;

static run intrinsic_in(int axis) { return axis == 0 ? INTRINSIC_X : INTRINSIC_Y; }

#define MIN_CONTENT UI_AVAIL_MIN_CONTENT
#define MAX_CONTENT UI_AVAIL_MAX_CONTENT
#define LOCAL_ITEMS 16u

typedef struct ctx {
    ui_scene *s;
    ui_text_context *text;
    float scale;
    ui_layout_stats *stats;
} ctx;

// The surface pseudo-container: a column with initial values otherwise.
static ui_layout_style root_style;

static void layout_styles(void) {
    if (root_style.direction == UI_FLEX_COLUMN) return;
    ui_layout_style_init(&root_style);
    root_style.direction = UI_FLEX_COLUMN;
}

static bool is_known(float v) { return !isnan(v); }
static bool is_definite(float avail) { return avail >= 0; }

static float resolve(ui_length l, float base) {
    if (l.unit == UI_LEN_PX) return l.value;
    if (l.unit == UI_LEN_PERCENT && is_known(base)) return l.value * base / 100.f;
    return NAN;
}

static float or_zero(float v) { return is_known(v) ? v : 0; }

static float clampf(float v, float lo, float hi) {
    // min wins over max when they conflict, as in CSS.
    if (is_known(hi) && v > hi) v = hi;
    if (is_known(lo) && v < lo) v = lo;
    return v;
}

// ---- Boxes --------------------------------------------------------------------------------

// What the algorithm needs of a node: its style and border, its in-flow children, its text.
typedef struct box {
    uint32_t slot;              // UI_NO_SLOT: the surface root
    const ui_layout_style *st;
    float border;               // uniform border width
    uint32_t first_child;
    ui_paragraph *text;         // a text leaf's paragraph (docs/layout.md), or NULL
} box;

static box node_box(ctx *c, uint32_t slot) {
    ui_node *n = &c->s->nodes[slot];
    return (box){slot, &n->computed.layout, n->computed.border_width, n->first_child, n->text};
}

static bool hidden(ctx *c, uint32_t slot) {
    return c->s->nodes[slot].computed.layout.display == UI_DISPLAY_NONE;
}

static bool absolute(ctx *c, uint32_t slot) {
    return c->s->nodes[slot].computed.layout.position == UI_POSITION_ABSOLUTE;
}

// Padding plus border per side (top, right, bottom, left); percentages resolve against
// the containing block's width.
static void padding_border(const box *b, float cb_width, float out[4]) {
    for (int i = 0; i < 4; i++) out[i] = fmaxf(or_zero(resolve(b->st->padding[i], cb_width)), 0) + b->border;
}

static float sum_axis(const float sides[4], int axis) {
    return axis == 0 ? sides[1] + sides[3] : sides[0] + sides[2];
}

// ---- Text --------------------------------------------------------------------------------

// The content size of a text leaf: its width from what is known or available (min-content,
// max-content, or fit-content in definite space), and its height at that width.
static void measure_text(ctx *c, ui_paragraph *p, const float known[2], const float avail[2],
                         float out[2], float *baseline) {
    if (!c->text) { // laid out without fonts (scene-level tests): the text takes no room
        out[0] = or_zero(known[0]);
        out[1] = or_zero(known[1]);
        *baseline = NAN;
        return;
    }
    float s = c->scale, w = known[0];
    ui_text_measure m = {0};
    if (!is_known(w)) {
        float min_w = 0;
        ui_paragraph_measure(p, c->text, s, INFINITY, &m);
        float max_w = m.width / s;
        if (avail[0] == MAX_CONTENT) {
            w = max_w;
        } else {
            ui_paragraph_min_content(p, c->text, s, &min_w);
            min_w /= s;
            w = avail[0] == MIN_CONTENT ? min_w : fminf(max_w, fmaxf(min_w, avail[0]));
        }
    }
    ui_paragraph_measure(p, c->text, s, w * s, &m);
    out[0] = w;
    out[1] = is_known(known[1]) ? known[1] : m.height / s;
    *baseline = m.baseline / s;
}

// ---- Computation with caching -------------------------------------------------------------

static void compute_box(ctx *c, const box *b, const float known[2], const float parent[2],
                        const float avail[2], run mode, float out[2], float *baseline);

static bool same(float a, float b) { return a == b || (isnan(a) && isnan(b)); }

static bool matches(const ui_layout_cache *e, const float known[2], const float parent[2],
                    const float avail[2], uint8_t intrinsic, bool baseline) {
    return e->used && e->intrinsic == intrinsic && (!baseline || e->has_baseline) &&
           same(e->known[0], known[0]) && same(e->known[1], known[1]) &&
           same(e->parent[0], parent[0]) && same(e->parent[1], parent[1]) &&
           same(e->avail[0], avail[0]) && same(e->avail[1], avail[1]);
}

// True if a node's own sizes depend on its parent's content size: a percentage size, min,
// max or padding. Otherwise that size is left out of its cache keys, so a parent measured
// first with an indefinite size and then a definite one still hits the cache.
static bool uses_percent(const ui_layout_style *st) {
    for (int a = 0; a < 2; a++) {
        if (st->size[a].unit == UI_LEN_PERCENT || st->min[a].unit == UI_LEN_PERCENT ||
            st->max[a].unit == UI_LEN_PERCENT)
            return true;
    }
    for (int i = 0; i < 4; i++) {
        if (st->padding[i].unit == UI_LEN_PERCENT) return true;
    }
    return false;
}

// A node's size for these inputs, from its cache when it has them. `perform` also lays out
// its children (and is cached separately: a measurement doesn't position anything).
static void compute(ctx *c, uint32_t slot, const float known[2], const float parent_size[2],
                    const float avail_in[2], run mode, float out[2], float *baseline) {
    ui_layout_node *l = &c->s->nodes[slot].layout;
    // Heights have one content size: the min-content and max-content block sizes are both
    // the height at the width given (CSS Sizing §5.1), and nothing here reads a min-content
    // constraint in y. So both share cache entries.
    float avail[2] = {avail_in[0], avail_in[1] == MIN_CONTENT ? MAX_CONTENT : avail_in[1]};
    static const float none[2] = {NAN, NAN};
    // Its style changes only with the node dirty, which empties the cache: until then the
    // answer found at the last computation holds.
    const float *parent = l->percent ? parent_size : none;
    bool perform = mode == PERFORM, baseline_wanted = mode == MEASURE_BASELINE || perform;
    uint8_t intrinsic = mode == INTRINSIC_X ? 1 : mode == INTRINSIC_Y ? 2 : 0;
    if (!l->dirty) {
        const ui_layout_cache *hit = NULL;
        if (matches(&l->final, known, parent, avail, intrinsic, baseline_wanted)) hit = &l->final;
        for (uint32_t i = 0; !hit && !perform && i < UI_LAYOUT_CACHE; i++) {
            if (matches(&l->measured[i], known, parent, avail, intrinsic, baseline_wanted))
                hit = &l->measured[i];
        }
        if (hit) {
            c->stats->cache_hits++;
            out[0] = hit->size[0];
            out[1] = hit->size[1];
            *baseline = hit->baseline;
            return;
        }
    } else {
        // Its inputs changed: nothing cached for it holds any more.
        memset(l->measured, 0, sizeof l->measured);
        l->final.used = false;
        l->dirty = false;
    }
    l->percent = uses_percent(&c->s->nodes[slot].computed.layout);
    parent = l->percent ? parent_size : none;
    c->stats->computed++;
    box b = node_box(c, slot);
    compute_box(c, &b, known, parent, avail, mode, out, baseline);
    ui_layout_cache e = {{known[0], known[1]}, {parent[0], parent[1]}, {avail[0], avail[1]},
                         {out[0], out[1]}, *baseline, true, intrinsic, baseline_wanted};
    if (perform) {
        l->final = e;
        l->baseline = *baseline;
    } else {
        l->measured[l->next] = e;
        l->next = (uint8_t)((l->next + 1) % UI_LAYOUT_CACHE);
    }
}

static void compute_item(ctx *c, const box *b, const float known[2], const float parent[2],
                         const float avail[2], run mode, float out[2], float *baseline) {
    compute(c, b->slot, known, parent, avail, mode, out, baseline);
}

// ---- Flex items ---------------------------------------------------------------------------

typedef struct item {
    box b;
    float margin[4];      // resolved; auto counts as 0 until alignment
    bool auto_margin[4];
    float pb[4];          // padding + border
    float size[2];        // specified border-box size; NaN: auto, or a percentage of nothing
    bool auto_size[2];    // the size computes to auto (only then does stretch apply)
    float min[2], max[2]; // border-box; min NaN: auto (resolved later), max NaN: none
    uint8_t align;        // align-self, resolved
    float basis;          // flex base size
    float hypo_main;      // hypothetical main size
    float target;         // main size after resolving flexible lengths
    float cross;          // cross size (hypothetical, then used)
    float baseline;       // from the border box's top
    float pos[2];         // border box offset within the container's border box
    bool frozen;
    float violation;
    uint32_t line;
    float cross_known;    // the cross size the main size was measured with (NaN: none)
    bool min_pending;     // automatic minimum not computed yet (it can't exceed the basis)
} item;

typedef struct line {
    uint32_t first, count;
    float cross;          // line cross size
    float max_ascent;     // baseline alignment
    float pos;            // offset of the line within the content box's cross axis
} line;

static float outer_main(const item *it, int m) { return it->target + sum_axis(it->margin, m); }

static bool main_reverse(const ui_layout_style *st) {
    return st->direction == UI_FLEX_ROW_REVERSE || st->direction == UI_FLEX_COLUMN_REVERSE;
}

// align-self as used: auto takes the container's align-items; normal means stretch.
static uint8_t used_align(const ui_layout_style *container, const ui_layout_style *self) {
    uint8_t a = self->align_self ? self->align_self : container->align_items;
    return a ? a : UI_FLEX_STRETCH;
}

static void init_item(item *it, const box *b, const ui_layout_style *cst, const float inner[2]) {
    memset(it, 0, sizeof *it);
    it->b = *b;
    const ui_layout_style *st = b->st;
    // Percent margins and padding resolve against the containing block's width.
    for (int i = 0; i < 4; i++) {
        it->auto_margin[i] = st->margin[i].unit == UI_LEN_AUTO;
        it->margin[i] = or_zero(resolve(st->margin[i], inner[0]));
    }
    padding_border(b, inner[0], it->pb);
    for (int a = 0; a < 2; a++) {
        it->auto_size[a] = st->size[a].unit == UI_LEN_AUTO;
        it->size[a] = resolve(st->size[a], inner[a]);
        it->min[a] = resolve(st->min[a], inner[a]);
        it->max[a] = resolve(st->max[a], inner[a]);
        if (st->max[a].unit == UI_LEN_NONE) it->max[a] = NAN;
        // A border box can't be smaller than its padding and border.
        float floor_pb = sum_axis(it->pb, a);
        if (is_known(it->size[a])) it->size[a] = fmaxf(it->size[a], floor_pb);
    }
    // aspect-ratio: an auto size follows the other axis (border-box ratio).
    float r = st->aspect_ratio;
    if (r > 0) {
        if (!is_known(it->size[0]) && is_known(it->size[1])) it->size[0] = it->size[1] * r;
        else if (!is_known(it->size[1]) && is_known(it->size[0])) it->size[1] = it->size[0] / r;
    }
    it->align = used_align(cst, st);
}

// The automatic minimum size of an item in the main axis (§4.5): the smaller of its
// specified (or transferred) size and its min-content size, never above its max size.
static float auto_min_main(ctx *c, item *it, int m, const float inner[2], const float cross_known) {
    float known[2] = {NAN, NAN}, avail[2], size[2], bl;
    known[1 - m] = cross_known;
    avail[m] = MIN_CONTENT;
    avail[1 - m] = is_known(cross_known) ? cross_known : MAX_CONTENT;
    compute_item(c, &it->b, known, inner, avail, intrinsic_in(m), size, &bl);
    float content = size[m];
    if (is_known(it->max[m])) content = fminf(content, it->max[m]);
    if (is_known(it->size[m])) return fminf(it->size[m], content);
    return content;
}

// ---- The container ------------------------------------------------------------------------

typedef struct layout_out {
    float size[2];
    float baseline;
} layout_out;

// Intrinsic contributions of an item in `axis` under a min- or max-content constraint
// (outer: margins included).
static float contribution(ctx *c, item *it, int axis, float constraint, const float inner[2]) {
    float v;
    if (is_known(it->size[axis])) {
        v = it->size[axis];
    } else {
        float known[2] = {NAN, NAN}, avail[2] = {MAX_CONTENT, MAX_CONTENT}, size[2], bl;
        avail[axis] = constraint;
        if (is_known(it->size[1 - axis])) known[1 - axis] = it->size[1 - axis];
        compute_item(c, &it->b, known, inner, avail, intrinsic_in(axis), size, &bl);
        v = size[axis];
    }
    v = clampf(v, is_known(it->min[axis]) ? it->min[axis] : sum_axis(it->pb, axis), it->max[axis]);
    return v + sum_axis(it->margin, axis);
}

static void position_absolute(ctx *c, const box *b, const ui_layout_style *cst, uint32_t slot,
                              const float size[2], const float pb[4], bool perform);

static void compute_box(ctx *c, const box *b, const float known[2], const float parent[2],
                        const float avail[2], run mode, float out[2], float *baseline) {
    const ui_layout_style *st = b->st;
    bool perform = mode == PERFORM;
    bool ignore[2] = {mode == INTRINSIC_X, mode == INTRINSIC_Y}; // own constraints, per axis
    int m = st->direction >= UI_FLEX_COLUMN ? 1 : 0, x = 1 - m;
    bool reverse = main_reverse(st);
    float pb[4];
    padding_border(b, parent[0], pb);
    float pbs[2] = {sum_axis(pb, 0), sum_axis(pb, 1)};

    // The node's own border-box size where already settled.
    float size[2] = {known[0], known[1]};
    float self_min[2], self_max[2];
    for (int a = 0; a < 2; a++) {
        self_min[a] = ignore[a] ? pbs[a] : fmaxf(or_zero(resolve(st->min[a], parent[a])), pbs[a]);
        self_max[a] = ignore[a] || st->max[a].unit == UI_LEN_NONE ? NAN : resolve(st->max[a], parent[a]);
        if (!is_known(size[a]) && !ignore[a]) {
            float spec = resolve(st->size[a], parent[a]);
            if (is_known(spec)) size[a] = clampf(fmaxf(spec, pbs[a]), self_min[a], self_max[a]);
        }
    }
    if (st->aspect_ratio > 0) {
        if (!is_known(size[0]) && is_known(size[1]) && !ignore[0]) size[0] = size[1] * st->aspect_ratio;
        else if (!is_known(size[1]) && is_known(size[0]) && !ignore[1]) size[1] = size[0] / st->aspect_ratio;
    }
    // Sizes settled so far are definite: percentages of children resolve against them.
    bool definite[2] = {is_known(size[0]), is_known(size[1])};

    // A text leaf: the paragraph fills the content box and sizes it; its width comes first,
    // within the node's min and max, then the height at that width (docs/layout.md).
    if (b->text) {
        float k[2] = {NAN, NAN}, a[2], content[2], bl;
        for (int i = 0; i < 2; i++) a[i] = is_definite(avail[i]) ? fmaxf(avail[i] - pbs[i], 0) : avail[i];
        if (!is_known(size[0])) {
            measure_text(c, b->text, k, a, content, &bl);
            size[0] = clampf(content[0] + pbs[0], self_min[0], self_max[0]);
            if (st->aspect_ratio > 0 && !ignore[1] && !is_known(size[1])) size[1] = size[0] / st->aspect_ratio;
        }
        k[0] = fmaxf(size[0] - pbs[0], 0);
        measure_text(c, b->text, k, a, content, &bl);
        if (!is_known(size[1])) size[1] = clampf(content[1] + pbs[1], self_min[1], self_max[1]);
        *baseline = is_known(bl) ? pb[0] + bl : NAN;
        if (perform) {
            // In-flow children lay out in the same content box, over the text.
            if (b->first_child != UI_NO_SLOT) {
                box children_only = *b;
                children_only.text = NULL;
                float sz[2], ignored;
                compute_box(c, &children_only, size, parent, size, PERFORM, sz, &ignored);
            }
            float *t = c->s->nodes[b->slot].layout.text;
            t[0] = pb[3];
            t[1] = pb[0];
            t[2] = k[0];
            t[3] = fmaxf(size[1] - pbs[1], 0);
        }
        out[0] = size[0];
        out[1] = size[1];
        return;
    }

    // Content box: settled sizes give definite inner sizes; otherwise the available space,
    // or a content constraint, passes on.
    float inner[2], inner_avail[2];
    for (int a = 0; a < 2; a++) {
        inner[a] = is_known(size[a]) ? fmaxf(size[a] - pbs[a], 0) : NAN;
        if (is_known(inner[a])) inner_avail[a] = inner[a];
        else if (is_definite(avail[a])) inner_avail[a] = fmaxf(avail[a] - pbs[a], 0);
        else inner_avail[a] = avail[a];
    }
    float gap_main = or_zero(resolve(st->gap[m], inner[m]));
    float gap_cross = or_zero(resolve(st->gap[x], inner[x]));

    // Items (§9.1): the in-flow children; absolutes are placed at the end.
    item local[LOCAL_ITEMS], *items = local;
    uint32_t count = 0, capacity = LOCAL_ITEMS, children = 0;
    for (uint32_t s = b->first_child; s != UI_NO_SLOT; s = c->s->nodes[s].next_sibling) children++;
    if (children > capacity && !(items = malloc(children * sizeof *items))) {
        items = local; // out of memory: lay out the first LOCAL_ITEMS only
        children = LOCAL_ITEMS;
    }
    for (uint32_t s = b->first_child; s != UI_NO_SLOT && count < children; s = c->s->nodes[s].next_sibling) {
        if (hidden(c, s)) {
            if (perform) memset(c->s->nodes[s].layout.rect, 0, sizeof c->s->nodes[s].layout.rect);
            continue;
        }
        if (absolute(c, s)) continue;
        box cb = node_box(c, s);
        init_item(&items[count++], &cb, st, inner);
    }
    (void)capacity;
    bool single_line = st->wrap == UI_FLEX_NOWRAP;

    // §9.2.3: flex base size and hypothetical main size.
    for (uint32_t i = 0; i < count; i++) {
        item *it = &items[i];
        // A stretched item in a single-line container with a definite cross size has a
        // definite cross size (§9.8.1).
        float cross_known = it->size[x];
        if (it->auto_size[x] && single_line && is_known(inner[x]) && it->align == UI_FLEX_STRETCH &&
            !it->auto_margin[x == 0 ? 1 : 0] && !it->auto_margin[x == 0 ? 3 : 2])
            cross_known = clampf(inner[x] - sum_axis(it->margin, x),
                                 is_known(it->min[x]) ? it->min[x] : sum_axis(it->pb, x), it->max[x]);
        float basis = resolve(it->b.st->basis, inner[m]);
        bool explicit_basis = is_known(basis);
        if (!is_known(basis)) basis = it->size[m];
        if (!is_known(basis) && it->b.st->aspect_ratio > 0 && is_known(cross_known))
            basis = m == 0 ? cross_known * it->b.st->aspect_ratio : cross_known / it->b.st->aspect_ratio;
        if (!is_known(basis)) {
            float k[2] = {NAN, NAN}, a[2], sz[2], bl;
            k[x] = cross_known;
            a[m] = avail[m] == MIN_CONTENT && !is_known(inner[m]) ? MIN_CONTENT : MAX_CONTENT;
            a[x] = is_known(cross_known) ? cross_known : inner_avail[x];
            compute_item(c, &it->b, k, inner, a, intrinsic_in(m), sz, &bl);
            basis = sz[m];
        }
        it->basis = fmaxf(basis, sum_axis(it->pb, m));
        it->cross_known = cross_known;
        // The automatic minimum (§4.5) is never above a basis taken from the item's size,
        // ratio or content, so it matters only when the line shrinks: it is computed then.
        // An explicit flex-basis can be below it, so that case needs it now.
        if (!is_known(it->min[m])) {
            if (explicit_basis) it->min[m] = auto_min_main(c, it, m, inner, cross_known);
            else it->min_pending = true;
        }
        it->min[m] = is_known(it->min[m]) ? fmaxf(it->min[m], sum_axis(it->pb, m)) : sum_axis(it->pb, m);
        if (!is_known(it->min[x])) it->min[x] = sum_axis(it->pb, x);
        it->hypo_main = clampf(it->basis, it->min[m], it->max[m]);
        it->cross = cross_known;
    }

    // §9.2.4: the container's main size.
    if (!is_known(inner[m])) {
        float content = 0;
        if (m == 0 && !is_definite(avail[0])) {
            // Chrome's intrinsic inline size of a row container: the sum of the items'
            // contributions (multi-line: min-content is the largest one).
            for (uint32_t i = 0; i < count; i++) {
                float v = contribution(c, &items[i], 0, avail[0], inner);
                content = !single_line && avail[0] == MIN_CONTENT ? fmaxf(content, v) : content + v;
            }
            if (count && (single_line || avail[0] != MIN_CONTENT)) content += gap_main * (float)(count - 1);
        } else {
            for (uint32_t i = 0; i < count; i++) content += items[i].hypo_main + sum_axis(items[i].margin, m);
            if (count) content += gap_main * (float)(count - 1);
            // A row container in definite available space is as wide as the space allows,
            // within its contents (fit-content).
            if (m == 0 && is_definite(inner_avail[0])) {
                float min_c = 0, max_c = 0;
                for (uint32_t i = 0; i < count; i++) {
                    float lo = contribution(c, &items[i], 0, MIN_CONTENT, inner);
                    float hi = contribution(c, &items[i], 0, MAX_CONTENT, inner);
                    min_c = single_line ? min_c + lo : fmaxf(min_c, lo);
                    max_c += hi;
                }
                if (count) {
                    max_c += gap_main * (float)(count - 1);
                    if (single_line) min_c += gap_main * (float)(count - 1);
                }
                content = fminf(max_c, fmaxf(min_c, inner_avail[0]));
            }
        }
        size[m] = clampf(content + pbs[m], self_min[m], self_max[m]);
        inner[m] = fmaxf(size[m] - pbs[m], 0);
    }
    // Intrinsic callers read only the measured axis: in the main axis, this is it.
    if ((m == 0 && ignore[0]) || (m == 1 && ignore[1])) {
        out[m] = size[m];
        out[x] = or_zero(size[x]);
        *baseline = NAN;
        if (items != local) free(items);
        return;
    }

    // §9.3.5: lines.
    line local_lines[LOCAL_ITEMS], *lines = local_lines;
    uint32_t line_count = 0;
    if (count > LOCAL_ITEMS && !(lines = malloc(count * sizeof *lines))) lines = local_lines, count = LOCAL_ITEMS;
    for (uint32_t i = 0; i < count;) {
        line *ln = &lines[line_count++];
        *ln = (line){.first = i};
        float used = 0;
        do {
            float w = items[i].hypo_main + sum_axis(items[i].margin, m);
            if (!single_line && ln->count && used + gap_main + w > inner[m] + 1e-3f) break;
            used += (ln->count ? gap_main : 0) + w;
            items[i].line = line_count - 1;
            ln->count++;
            i++;
        } while (i < count);
    }

    // §9.7: resolve the flexible lengths of each line.
    for (uint32_t li = 0; li < line_count; li++) {
        line *ln = &lines[li];
        item *first = &items[ln->first];
        float used = gap_main * (float)(ln->count ? ln->count - 1 : 0);
        for (uint32_t k = 0; k < ln->count; k++) used += first[k].hypo_main + sum_axis(first[k].margin, m);
        bool grow = used < inner[m];
        for (uint32_t k = 0; !grow && k < ln->count; k++) {
            item *it = &first[k];
            if (!it->min_pending) continue;
            it->min_pending = false;
            it->min[m] = fmaxf(auto_min_main(c, it, m, inner, it->cross_known), sum_axis(it->pb, m));
        }
        // Size inflexible items.
        for (uint32_t k = 0; k < ln->count; k++) {
            item *it = &first[k];
            it->frozen = (grow ? it->b.st->grow == 0 : it->b.st->shrink == 0) ||
                         (grow && it->basis > it->hypo_main) || (!grow && it->basis < it->hypo_main);
            it->target = it->frozen ? it->hypo_main : it->basis;
        }
        float initial_free = inner[m] - gap_main * (float)(ln->count ? ln->count - 1 : 0);
        for (uint32_t k = 0; k < ln->count; k++) {
            item *it = &first[k];
            initial_free -= sum_axis(it->margin, m) + (it->frozen ? it->target : it->basis);
        }
        for (;;) {
            bool unfrozen = false;
            float free_space = inner[m] - gap_main * (float)(ln->count ? ln->count - 1 : 0);
            float flex_sum = 0;
            for (uint32_t k = 0; k < ln->count; k++) {
                item *it = &first[k];
                free_space -= sum_axis(it->margin, m) + (it->frozen ? it->target : it->basis);
                if (!it->frozen) {
                    unfrozen = true;
                    flex_sum += grow ? it->b.st->grow : it->b.st->shrink;
                }
            }
            if (!unfrozen) break;
            // A sum of flex factors below 1 takes only that share of the initial free space.
            if (flex_sum < 1 && fabsf(initial_free * flex_sum) < fabsf(free_space))
                free_space = initial_free * flex_sum;
            // Shrinking is weighted by the inner (content-box) flex base size.
            float scaled_sum = 0;
            for (uint32_t k = 0; k < ln->count; k++) {
                if (!first[k].frozen)
                    scaled_sum += first[k].b.st->shrink * (first[k].basis - sum_axis(first[k].pb, m));
            }
            float total_violation = 0;
            for (uint32_t k = 0; k < ln->count; k++) {
                item *it = &first[k];
                if (it->frozen) continue;
                float t = it->basis;
                if (grow && flex_sum > 0) {
                    t += free_space * it->b.st->grow / flex_sum;
                } else if (!grow && scaled_sum > 0) {
                    t += free_space * it->b.st->shrink * (it->basis - sum_axis(it->pb, m)) / scaled_sum;
                }
                float clamped = clampf(t, it->min[m], it->max[m]);
                it->violation = clamped - t;
                total_violation += it->violation;
                it->target = clamped;
            }
            // Freeze: all if no violation, the min violations if positive, max if negative.
            // Each round freezes at least one item, so the loop ends; with sizes so extreme
            // that the violations aren't numbers (inf - inf), everything freezes at once.
            bool froze = false;
            for (uint32_t k = 0; k < ln->count; k++) {
                item *it = &first[k];
                if (it->frozen) continue;
                if (total_violation == 0 || (total_violation > 0 && it->violation > 0) ||
                    (total_violation < 0 && it->violation < 0))
                    it->frozen = froze = true;
            }
            for (uint32_t k = 0; !froze && k < ln->count; k++) first[k].frozen = true;
        }
    }

    // §9.4.7: hypothetical cross sizes, with the used main size. Children's percentage
    // heights resolve only against definite heights (§9.8): a main size is definite when
    // the container's is, a cross size when specified or stretched.
    float parent_inner[2] = {inner[0], definite[1] ? inner[1] : NAN};
    for (uint32_t i = 0; i < count; i++) {
        item *it = &items[i];
        float k[2], a[2], sz[2], bl = NAN;
        k[m] = it->target;
        k[x] = it->cross; // known when specified, or stretched in a definite single line
        a[m] = it->target;
        a[x] = is_known(inner[x]) ? fmaxf(inner[x] - sum_axis(it->margin, x), 0) : inner_avail[x];
        // A known cross size is the hypothetical one: measuring is needed only for a baseline
        // (a baseline-aligned item, or the first item when this container's baseline is
        // wanted).
        bool wants = it->align == UI_FLEX_BASELINE || (i == 0 && mode == MEASURE_BASELINE);
        if (is_known(k[x]) && !wants) {
            it->cross = clampf(k[x], it->min[x], it->max[x]);
            it->baseline = NAN;
            continue;
        }
        compute_item(c, &it->b, k, parent_inner, a, wants ? MEASURE_BASELINE : MEASURE, sz, &bl);
        it->cross = clampf(sz[x], it->min[x], it->max[x]);
        it->baseline = bl;
    }

    // §9.4.8: line cross sizes, with baseline alignment. In a row the baseline is the
    // item's first baseline, else its bottom border edge; a column's items have none in
    // the cross axis, so it is synthesized from their start (left) border edge.
    int cs = x == 0 ? 3 : 0, ce = x == 0 ? 1 : 2; // cross-axis margin sides
    for (uint32_t li = 0; li < line_count; li++) {
        line *ln = &lines[li];
        float max_outer = 0, max_ascent = 0, max_descent = 0;
        for (uint32_t k = 0; k < ln->count; k++) {
            item *it = &items[ln->first + k];
            float outer = it->cross + sum_axis(it->margin, x);
            if (it->align == UI_FLEX_BASELINE && !it->auto_margin[cs] && !it->auto_margin[ce]) {
                float bl = m == 1 ? 0 : is_known(it->baseline) ? it->baseline : it->cross;
                float ascent = bl + it->margin[cs];
                max_ascent = fmaxf(max_ascent, ascent);
                max_descent = fmaxf(max_descent, outer - ascent);
            } else {
                max_outer = fmaxf(max_outer, outer);
            }
        }
        ln->max_ascent = max_ascent;
        ln->cross = fmaxf(max_outer, max_ascent + max_descent);
    }
    if (single_line && line_count) {
        if (is_known(inner[x])) {
            lines[0].cross = inner[x];
        } else {
            float lo = fmaxf(self_min[x] - pbs[x], 0);
            float hi = is_known(self_max[x]) ? self_max[x] - pbs[x] : NAN;
            lines[0].cross = clampf(lines[0].cross, lo, hi);
        }
    }

    // The container's cross size, when not settled: its lines.
    if (!is_known(inner[x])) {
        float content = 0;
        for (uint32_t li = 0; li < line_count; li++) content += lines[li].cross;
        if (line_count) content += gap_cross * (float)(line_count - 1);
        size[x] = clampf(content + pbs[x], self_min[x], self_max[x]);
        inner[x] = fmaxf(size[x] - pbs[x], 0);
        if (single_line && line_count) lines[0].cross = inner[x];
    }
    // Intrinsic callers read only the measured axis: in the cross axis, this is it.
    if (ignore[x]) {
        out[0] = size[0];
        out[1] = size[1];
        *baseline = NAN;
        if (items != local) free(items);
        if (lines != local_lines) free(lines);
        return;
    }

    // §9.4.9: align-content: stretch grows the lines into free cross space.
    float lines_total = gap_cross * (float)(line_count ? line_count - 1 : 0);
    for (uint32_t li = 0; li < line_count; li++) lines_total += lines[li].cross;
    float cross_free = inner[x] - lines_total;
    uint8_t ac = st->align_content ? st->align_content : UI_FLEX_STRETCH;
    if (!single_line && ac == UI_FLEX_STRETCH && cross_free > 0 && line_count) {
        for (uint32_t li = 0; li < line_count; li++) lines[li].cross += cross_free / (float)line_count;
        cross_free = 0;
    }

    // §9.4.11: stretched items take the line's cross size.
    for (uint32_t i = 0; i < count; i++) {
        item *it = &items[i];
        int start = x == 0 ? 3 : 0, end = x == 0 ? 1 : 2;
        if (it->align == UI_FLEX_STRETCH && it->auto_size[x] && !it->auto_margin[start] &&
            !it->auto_margin[end])
            it->cross = clampf(lines[it->line].cross - sum_axis(it->margin, x), it->min[x], it->max[x]);
    }

    // §9.6.16: align-content places the lines.
    {
        float pos = 0, between = 0;
        if (!single_line) {
            uint8_t a = ac;
            if (cross_free < 0 && (a == UI_FLEX_SPACE_BETWEEN || a == UI_FLEX_STRETCH)) a = UI_FLEX_START;
            if (cross_free < 0 && (a == UI_FLEX_SPACE_AROUND || a == UI_FLEX_SPACE_EVENLY)) a = UI_FLEX_CENTER;
            float n = (float)line_count;
            switch (a) {
            case UI_FLEX_END: pos = cross_free; break;
            case UI_FLEX_CENTER: pos = cross_free / 2; break;
            case UI_FLEX_SPACE_BETWEEN: between = line_count > 1 ? cross_free / (n - 1) : 0; break;
            case UI_FLEX_SPACE_AROUND:
                between = cross_free / n;
                pos = between / 2;
                break;
            case UI_FLEX_SPACE_EVENLY:
                between = cross_free / (n + 1);
                pos = between;
                break;
            default: break;
            }
        }
        bool wrap_reverse = st->wrap == UI_FLEX_WRAP_REVERSE;
        for (uint32_t k = 0; k < line_count; k++) {
            uint32_t li = wrap_reverse ? line_count - 1 - k : k;
            lines[li].pos = pos;
            pos += lines[li].cross + gap_cross + between;
        }
    }

    // §9.5.12-13 and §9.6.14-15: main and cross axis alignment within each line.
    int m_start = m == 0 ? 3 : 0, m_end = m == 0 ? 1 : 2; // margin sides
    int x_start = x == 0 ? 3 : 0, x_end = x == 0 ? 1 : 2;
    for (uint32_t li = 0; li < line_count; li++) {
        line *ln = &lines[li];
        float used = gap_main * (float)(ln->count ? ln->count - 1 : 0);
        uint32_t autos = 0;
        for (uint32_t k = 0; k < ln->count; k++) {
            item *it = &items[ln->first + k];
            used += outer_main(it, m);
            autos += it->auto_margin[m_start] + it->auto_margin[m_end];
        }
        float free_space = inner[m] - used, pos = 0, between = 0, auto_share = 0;
        if (autos && free_space > 0) {
            auto_share = free_space / (float)autos;
        } else {
            uint8_t j = st->justify;
            if (free_space < 0 && j == UI_FLEX_SPACE_BETWEEN) j = UI_FLEX_START;
            if (free_space < 0 && (j == UI_FLEX_SPACE_AROUND || j == UI_FLEX_SPACE_EVENLY)) j = UI_FLEX_CENTER;
            float n = (float)ln->count;
            switch (j) {
            case UI_FLEX_END: pos = free_space; break;
            case UI_FLEX_CENTER: pos = free_space / 2; break;
            case UI_FLEX_SPACE_BETWEEN: between = ln->count > 1 ? free_space / (n - 1) : 0; break;
            case UI_FLEX_SPACE_AROUND:
                between = free_space / n;
                pos = between / 2;
                break;
            case UI_FLEX_SPACE_EVENLY:
                between = free_space / (n + 1);
                pos = between;
                break;
            default: break;
            }
        }
        for (uint32_t k = 0; k < ln->count; k++) {
            item *it = &items[ln->first + k];
            float before = it->margin[m_start] + (it->auto_margin[m_start] ? auto_share : 0);
            float after = it->margin[m_end] + (it->auto_margin[m_end] ? auto_share : 0);
            float main_pos = pos + before;
            // Reversed directions lay items out from the main end.
            it->pos[m] = reverse ? inner[m] - main_pos - it->target : main_pos;
            pos += before + it->target + after + gap_main + between;

            // Cross axis.
            float outer_cross = it->cross + sum_axis(it->margin, x);
            float lfree = ln->cross - outer_cross, cpos;
            uint32_t cross_autos = it->auto_margin[x_start] + it->auto_margin[x_end];
            if (cross_autos && lfree > 0) {
                cpos = it->auto_margin[x_start] ? lfree / (float)cross_autos : 0;
            } else {
                switch (it->align) {
                case UI_FLEX_END: cpos = lfree; break;
                case UI_FLEX_CENTER: cpos = lfree / 2; break;
                case UI_FLEX_BASELINE: {
                    float bl = m == 1 ? 0 : is_known(it->baseline) ? it->baseline : it->cross;
                    cpos = ln->max_ascent - bl - it->margin[x_start];
                    break;
                }
                default: cpos = 0; break;
                }
            }
            float cross_in_line = cpos + it->margin[x_start];
            if (st->wrap == UI_FLEX_WRAP_REVERSE) cross_in_line = ln->cross - cross_in_line - it->cross;
            it->pos[x] = ln->pos + cross_in_line;
        }
    }

    // Convert to the container's border box, lay out the items, and find the baseline.
    float content_origin[2] = {pb[3], pb[0]};
    *baseline = NAN;
    float first_baseline = NAN;
    for (uint32_t i = 0; i < count; i++) {
        item *it = &items[i];
        it->pos[0] += content_origin[0];
        it->pos[1] += content_origin[1];
        // The item's sizes: known to it when definite, else the room it gets.
        float k[2], a[2], sz[2], bl = NAN;
        bool stretched = it->align == UI_FLEX_STRETCH && it->auto_size[x] &&
                         !it->auto_margin[x_start] && !it->auto_margin[x_end];
        // Widths are settled once laid out (percentages are cyclic only while measuring
        // intrinsic sizes); heights only when definite.
        a[m] = it->target;
        a[x] = it->cross;
        k[m] = m == 0 || definite[m] || is_known(it->size[m]) ? it->target : NAN;
        k[x] = x == 0 || stretched || is_known(it->size[x]) ? it->cross : NAN;
        // Measuring only needs baselines, which the hypothetical cross sizes gave.
        if (perform) {
            compute_item(c, &it->b, k, parent_inner, a, PERFORM, sz, &bl);
            it->baseline = bl;
        }
        if (perform) {
            float *r = c->s->nodes[it->b.slot].layout.rect;
            r[0] = it->pos[0];
            r[1] = it->pos[1];
            r[2] = m == 0 ? it->target : it->cross;
            r[3] = m == 0 ? it->cross : it->target;
            memcpy(c->s->nodes[it->b.slot].rect, r, 4 * sizeof *r); // drawn where laid out
        }
        // The container's first baseline: the first baseline-aligned item of the first
        // line, else the first item's, synthesized from its bottom border edge when it has
        // none (§8.5).
        if (is_known(it->baseline) && it->line == 0 && it->align == UI_FLEX_BASELINE && m == 0 &&
            !is_known(*baseline))
            *baseline = it->pos[1] + it->baseline;
        if (i == 0)
            first_baseline = it->pos[1] + (is_known(it->baseline) ? it->baseline : m == 0 ? it->cross : it->target);
    }
    if (!is_known(*baseline)) *baseline = first_baseline;

    out[0] = size[0];
    out[1] = size[1];
    if (perform) {
        if (b->slot != UI_NO_SLOT) memset(c->s->nodes[b->slot].layout.text, 0, 4 * sizeof(float));
        for (uint32_t s = b->first_child; s != UI_NO_SLOT; s = c->s->nodes[s].next_sibling) {
            if (!hidden(c, s) && absolute(c, s)) position_absolute(c, b, st, s, size, pb, perform);
        }
    }
    if (items != local) free(items);
    if (lines != local_lines) free(lines);
}

// ---- Absolute positioning -----------------------------------------------------------------

// An absolutely positioned child, in the padding box of its parent (every node is a
// containing block). Insets place it; with neither inset on an axis it sits where a sole
// flex item would (its static position).
static void position_absolute(ctx *c, const box *b, const ui_layout_style *cst, uint32_t slot,
                              const float size[2], const float pb[4], bool perform) {
    (void)b;
    ui_node *n = &c->s->nodes[slot];
    const ui_layout_style *st = &n->computed.layout;
    float border = b->border;
    float cb[2] = {fmaxf(size[0] - 2 * border, 0), fmaxf(size[1] - 2 * border, 0)};
    float inset[4], margin[4];
    bool auto_margin[4];
    for (int i = 0; i < 4; i++) {
        inset[i] = resolve(st->inset[i], cb[i % 2 == 1 ? 0 : 1]);
        auto_margin[i] = st->margin[i].unit == UI_LEN_AUTO;
        margin[i] = or_zero(resolve(st->margin[i], cb[0]));
    }
    box nb = node_box(c, slot);
    float npb[4];
    padding_border(&nb, cb[0], npb);
    float known[2], avail[2];
    for (int a = 0; a < 2; a++) {
        int lo = a == 0 ? 3 : 0, hi = a == 0 ? 1 : 2;
        known[a] = resolve(st->size[a], cb[a]);
        if (is_known(known[a])) known[a] = fmaxf(known[a], sum_axis(npb, a));
        float room = cb[a] - or_zero(inset[lo]) - or_zero(inset[hi]) - margin[lo] - margin[hi];
        if (!is_known(known[a]) && is_known(inset[lo]) && is_known(inset[hi])) known[a] = fmaxf(room, 0);
        avail[a] = fmaxf(room, 0);
        float lo_size = fmaxf(or_zero(resolve(st->min[a], cb[a])), sum_axis(npb, a));
        float hi_size = st->max[a].unit == UI_LEN_NONE ? NAN : resolve(st->max[a], cb[a]);
        if (is_known(known[a])) known[a] = clampf(known[a], lo_size, hi_size);
    }
    if (st->aspect_ratio > 0) {
        if (!is_known(known[0]) && is_known(known[1])) known[0] = known[1] * st->aspect_ratio;
        else if (!is_known(known[1]) && is_known(known[0])) known[1] = known[0] / st->aspect_ratio;
    }
    float sz[2], bl;
    float parent[2] = {cb[0], cb[1]};
    compute(c, slot, known, parent, avail, MEASURE, sz, &bl);
    int m = cst->direction >= UI_FLEX_COLUMN ? 1 : 0;
    float pos[2];
    for (int a = 0; a < 2; a++) {
        int lo = a == 0 ? 3 : 0, hi = a == 0 ? 1 : 2;
        float room = cb[a] - sz[a] - margin[lo] - margin[hi];
        if (is_known(inset[lo])) {
            float extra = is_known(inset[hi]) ? room - inset[lo] - inset[hi] : 0;
            float shift = 0;
            if (extra > 0 && (auto_margin[lo] || auto_margin[hi]))
                shift = auto_margin[lo] && auto_margin[hi] ? extra / 2 : auto_margin[lo] ? extra : 0;
            pos[a] = border + inset[lo] + margin[lo] + shift;
        } else if (is_known(inset[hi])) {
            pos[a] = border + cb[a] - inset[hi] - margin[hi] - sz[a];
        } else {
            // Static position: as the container's sole item, within its content box.
            float content = size[a] - sum_axis(pb, a), start = a == 0 ? pb[3] : pb[0];
            float free_space = content - sz[a] - margin[lo] - margin[hi];
            uint8_t align;
            bool reverse_axis = false;
            if (a == m) {
                align = cst->justify;
                reverse_axis = main_reverse(cst);
                if (align == UI_FLEX_SPACE_BETWEEN) align = UI_FLEX_START;
                if (align == UI_FLEX_SPACE_AROUND || align == UI_FLEX_SPACE_EVENLY) align = UI_FLEX_CENTER;
            } else {
                align = used_align(cst, st);
                reverse_axis = cst->wrap == UI_FLEX_WRAP_REVERSE;
            }
            float off = align == UI_FLEX_END ? free_space : align == UI_FLEX_CENTER ? free_space / 2 : 0;
            if (reverse_axis && align != UI_FLEX_CENTER) off = free_space - off;
            pos[a] = start + margin[lo] + off;
        }
    }
    float done[2];
    compute(c, slot, (float[]){sz[0], sz[1]}, parent, (float[]){sz[0], sz[1]}, perform ? PERFORM : MEASURE,
            done, &bl);
    n->layout.rect[0] = pos[0];
    n->layout.rect[1] = pos[1];
    n->layout.rect[2] = done[0];
    n->layout.rect[3] = done[1];
    memcpy(n->rect, n->layout.rect, sizeof n->rect); // drawn where laid out
}

// ---- Entry point --------------------------------------------------------------------------

// Lays out a surface's tree: the root is a column container the surface's size.
void ui_layout_root(ui_scene *s, uint32_t root, float width, float height, float scale,
                    ui_text_context *text, ui_layout_stats *stats) {
    ctx c = {s, text, scale, stats};
    stats->passes++;
    layout_styles();
    box rb = {UI_NO_SLOT, &root_style, 0, s->roots[root].first_child, NULL};
    float known[2] = {width, height}, parent[2] = {width, height}, out[2], bl;
    compute_box(&c, &rb, known, parent, known, PERFORM, out, &bl);
}
