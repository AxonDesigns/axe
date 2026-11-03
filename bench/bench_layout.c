// Layout benchmarks. Meaningful in release builds:
//   cmake --build --preset release && build/release/bench/bench_layout
// The tree: a wrapping grid of 1,429 cards on a 1600x900 surface, each a column of a title,
// a paragraph and a footer row (a label, a button, an icon): 10,005 nodes with the root and
// the grid, 4,287 of them text. Text is shaped before timing, as the worker threads do
// before a buffer applies.
//  1. Full layout: cold (nothing measured yet), then warm (every node's layout cache
//     dropped, text measurements kept, as when every container changes size).
//  2. Incremental relayout after one paragraph's text changes: resolve and lay out.
//     Target: under 1 ms.
//  3. Style resolution of all 10,005 nodes after the token their style uses changes.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/scene.h"
#include "runtime/common.h"

#define CARDS 1429u
#define FONT AXE_SOURCE_DIR "/tests/fonts/NotoSans-Regular-Static.ttf"

static ui_logger quiet = {UI_LOG_ERROR};
static ui_font_registry *fonts;
static ui_text_context text;
static const ui_handle surface = {(uint32_t)UI_HANDLE_SURFACE << UI_HANDLE_SLOT_BITS, 1};

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

// Median and best of n runs, in ms. The best is the steadier figure on a busy machine.
static double median_ms(uint64_t *ns, int n) {
    qsort(ns, (size_t)n, sizeof *ns, cmp_u64);
    return (double)ns[n / 2] / 1e6;
}

static double best_ms(const uint64_t *ns, int n) {
    uint64_t best = ns[0];
    for (int i = 1; i < n; i++) best = ns[i] < best ? ns[i] : best;
    return (double)best / 1e6;
}

static void apply(ui_scene *s, ui_cmd_writer *w) {
    uint8_t *bytes;
    size_t size;
    ui_cmd_writer_take(w, &bytes, &size);
    if (ui_scene_apply(s, bytes, size, 0) != UI_OK) abort();
    free(bytes);
}

static void set_text(ui_cmd_writer *w, ui_handle h, const char *s, float size) {
    static const char *const fam[] = {"Noto Sans"};
    ui_text_desc t = {.text = s, .length = (uint32_t)strlen(s),
                      .style = {.family = 0, .weight = 400, .size = size, .color = {0, 0, 0, 1}},
                      .families = fam, .family_count = 1, .line_height = 1.25f};
    if (ui_cmd_set_text(w, h, &t) != UI_OK) abort();
}

// Shapes every paragraph, as preparation does, without measuring any line layout that
// will ask for.
static void shape_all(ui_scene *s) {
    for (uint32_t slot = 0; slot < s->handles.used; slot++) {
        ui_result r;
        if (s->handles.alive[slot] && s->nodes[slot].text)
            ui_paragraph_layout_get(s->nodes[slot].text, &text, 1, INFINITY, &r);
    }
}

static uint32_t paragraph_slot;

static void build(ui_scene *s, ui_cmd_writer *w) {
    ui_cmd_set_token(w, 1, ui_rgba(0.95f, 0.95f, 0.97f, 1));
    ui_style_decl base[] = {{UI_STYLE_BACKGROUND_COLOR, ui_token_ref(1)}};
    ui_cmd_define_style(w, 1, &(ui_style_block_desc){0, base, 1}, 1);
    ui_style_decl card[] = {{UI_STYLE_WIDTH, ui_px(180)}, {UI_STYLE_FLEX_DIRECTION, ui_keyword(UI_FLEX_COLUMN)},
                            {UI_STYLE_PADDING_TOP, ui_px(8)}, {UI_STYLE_PADDING_RIGHT, ui_px(8)},
                            {UI_STYLE_PADDING_BOTTOM, ui_px(8)}, {UI_STYLE_PADDING_LEFT, ui_px(8)},
                            {UI_STYLE_ROW_GAP, ui_px(4)}, {UI_STYLE_BORDER_WIDTH, ui_px(1)}};
    ui_cmd_define_style(w, 2, &(ui_style_block_desc){0, card, 8}, 1);
    ui_style_decl footer[] = {{UI_STYLE_JUSTIFY_CONTENT, ui_keyword(UI_FLEX_SPACE_BETWEEN)},
                              {UI_STYLE_ALIGN_ITEMS, ui_keyword(UI_FLEX_CENTER)}};
    ui_cmd_define_style(w, 3, &(ui_style_block_desc){0, footer, 2}, 1);
    uint32_t slot = 1;
    ui_handle root = {slot++, 1}, grid = {slot++, 1};
    ui_cmd_create_node(w, root, surface);
    ui_cmd_set_props(w, root, &(ui_style_decl){UI_STYLE_FLEX_GROW, ui_number(1)}, 1);
    ui_cmd_create_node(w, grid, root);
    ui_cmd_set_props(w, grid, (ui_style_decl[]){{UI_STYLE_FLEX_WRAP, ui_keyword(UI_FLEX_WRAP)},
                                                {UI_STYLE_ROW_GAP, ui_px(8)}, {UI_STYLE_COLUMN_GAP, ui_px(8)},
                                                {UI_STYLE_PADDING_TOP, ui_px(8)}, {UI_STYLE_PADDING_LEFT, ui_px(8)}}, 5);
    char buf[160];
    for (uint32_t i = 0; i < CARDS; i++) {
        ui_handle card_h = {slot++, 1};
        ui_cmd_create_node(w, card_h, grid);
        ui_cmd_set_styles(w, card_h, (uint32_t[]){1, 2}, 2);
        ui_handle title = {slot++, 1}, para = {slot++, 1}, foot = {slot++, 1};
        ui_handle label = {slot++, 1}, button = {slot++, 1}, icon = {slot++, 1};
        if (i == CARDS / 2) paragraph_slot = para.index;
        ui_handle parts[] = {title, para, foot};
        for (int k = 0; k < 3; k++) {
            ui_cmd_create_node(w, parts[k], card_h);
            ui_cmd_set_styles(w, parts[k], (uint32_t[]){1, k == 2 ? 3u : 1u}, k == 2 ? 2 : 1);
        }
        snprintf(buf, sizeof buf, "Card number %u", i);
        set_text(w, title, buf, 16);
        snprintf(buf, sizeof buf, "Item %u has a short paragraph that wraps over a few lines in its card.", i);
        set_text(w, para, buf, 13);
        ui_cmd_create_node(w, label, foot);
        ui_cmd_set_styles(w, label, (uint32_t[]){1}, 1);
        snprintf(buf, sizeof buf, "#%u", i);
        set_text(w, label, buf, 12);
        ui_cmd_create_node(w, button, foot);
        ui_cmd_set_styles(w, button, (uint32_t[]){1}, 1);
        ui_cmd_set_props(w, button, (ui_style_decl[]){{UI_STYLE_WIDTH, ui_px(40)}, {UI_STYLE_HEIGHT, ui_px(20)}}, 2);
        ui_cmd_create_node(w, icon, foot);
        ui_cmd_set_styles(w, icon, (uint32_t[]){1}, 1);
        ui_cmd_set_props(w, icon, (ui_style_decl[]){{UI_STYLE_WIDTH, ui_px(16)}, {UI_STYLE_HEIGHT, ui_px(16)}}, 2);
        if (ui_cmd_writer_op_count(w) > 20000) apply(s, w); // keep buffers modest
    }
    apply(s, w);
    ui_scene_resolve(s, 0);
}

static void mark_all_dirty(ui_scene *s) {
    for (uint32_t slot = 0; slot < s->handles.used; slot++) {
        if (s->handles.alive[slot]) s->nodes[slot].layout.dirty = true;
    }
    s->roots[0].layout_dirty = true;
}

int main(void) {
    ui_font_registry_create(&(ui_font_registry_desc){.log = &quiet}, &fonts);
    ui_font_registry_add_file(fonts, FONT, 0, NULL);
    ui_text_context_init(&text, fonts);
#ifndef NDEBUG
    printf("(debug build: numbers are not representative; use the release preset)\n");
#endif
    ui_scene s;
    ui_scene_init(&s, &quiet);
    ui_scene_add_root(&s, surface);
    ui_cmd_writer *w;
    ui_cmd_writer_create(&w);
    build(&s, w);
    shape_all(&s);
    uint32_t nodes = s.handles.live;

    // 1. Full layout.
    uint64_t t0 = ui_monotonic_ns();
    ui_scene_layout(&s, surface, 1600, 900, 1, &text);
    uint64_t cold = ui_monotonic_ns() - t0;
    uint64_t computed = s.layout_stats.computed;
    enum { RUNS = 15 };
    uint64_t warm[RUNS];
    for (int r = 0; r < RUNS; r++) {
        mark_all_dirty(&s);
        t0 = ui_monotonic_ns();
        ui_scene_layout(&s, surface, 1600, 900, 1, &text);
        warm[r] = ui_monotonic_ns() - t0;
    }
    printf("full layout   %u nodes: %.2f ms cold (%llu computations); with text measured %.2f ms median, "
           "%.2f ms best\n",
           nodes, (double)cold / 1e6, (unsigned long long)computed, median_ms(warm, RUNS), best_ms(warm, RUNS));

    // 2. One paragraph's text changes.
    enum { EDITS = 101 };
    uint64_t inc[EDITS], inc_computed = 0, inc_hits = 0;
    char buf[160];
    for (int r = 0; r < EDITS; r++) {
        snprintf(buf, sizeof buf, "Edited %d times: this paragraph now reads differently%s.", r,
                 r % 2 ? " and runs a little longer than before" : "");
        set_text(w, (ui_handle){paragraph_slot, 1}, buf, 13);
        apply(&s, w);
        ui_result res;
        ui_paragraph_layout_get(s.nodes[paragraph_slot].text, &text, 1, INFINITY, &res); // shaped ahead
        uint64_t before = s.layout_stats.computed, hits = s.layout_stats.cache_hits;
        t0 = ui_monotonic_ns();
        ui_scene_resolve(&s, 0);
        ui_scene_layout(&s, surface, 1600, 900, 1, &text);
        inc[r] = ui_monotonic_ns() - t0;
        inc_computed += s.layout_stats.computed - before;
        inc_hits += s.layout_stats.cache_hits - hits;
    }
    printf("incremental   one paragraph edited: %.3f ms median, %.3f ms best (%llu computations, %llu cache "
           "hits per edit)\n",
           median_ms(inc, EDITS), best_ms(inc, EDITS), (unsigned long long)(inc_computed / EDITS),
           (unsigned long long)(inc_hits / EDITS));

    // 3. A token every node's style uses changes.
    uint64_t res_ns[RUNS];
    for (int r = 0; r < RUNS; r++) {
        ui_cmd_set_token(w, 1, ui_rgba(r % 2 ? 0.1f : 0.95f, 0.95f, 0.97f, 1));
        apply(&s, w);
        t0 = ui_monotonic_ns();
        ui_scene_resolve(&s, 0);
        res_ns[r] = ui_monotonic_ns() - t0;
    }
    printf("resolution    token change, %u nodes: %.3f ms median, %.3f ms best\n", nodes, median_ms(res_ns, RUNS),
           best_ms(res_ns, RUNS));

    ui_cmd_writer_destroy(w);
    ui_scene_destroy(&s);
    ui_text_context_free(&text);
    ui_font_registry_destroy(fonts);
    return 0;
}
