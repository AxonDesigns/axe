// Paragraph layout: line breaking (UAX #14 opportunities, emergency breaks), per-line bidi
// reordering (L1, L2), metrics with whole-pixel baselines, max lines with an ellipsis, and a
// two-level cache. All lengths are physical pixels; text offsets are UTF-8 byte offsets.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "text/shape.h"

typedef struct ui_layout_glyph {
    uint32_t glyph;
    uint32_t cluster;  // byte offset of the glyph's first character
    float x, y;        // pen position relative to the paragraph origin; y is the baseline
    float advance;
    uint32_t style;    // style index of the cluster (color)
} ui_layout_glyph;

// A piece of a shaped run on one line, in visual order. Glyph order within it is visual too.
typedef struct ui_layout_run {
    ui_font_key font;
    uint32_t start, end;   // bytes
    uint8_t level;         // after L1 (odd = RTL)
    bool ellipsis;         // the ellipsis, not text; start == end == the truncation point
    float x, width;        // relative to the paragraph origin
    uint32_t glyph_start, glyph_count;
} ui_layout_run;

typedef struct ui_line {
    uint32_t start, end;   // bytes, including trailing whitespace and the line break itself
    float top, height;     // line box, px from the paragraph top (height is whole px)
    float ascent, descent;
    float baseline;        // whole px
    float x, width;        // visible content (trailing whitespace hangs outside)
    uint32_t run_start, run_count; // into ui_paragraph_layout.runs, visual order
    bool ellipsized;
} ui_line;

typedef struct ui_paragraph_layout {
    float max_width;       // what it was laid out for (INFINITY = unbounded)
    float width, height;   // the widest line's visible width; total height
    uint8_t para_level;    // base direction of the first paragraph (alignment)
    bool truncated;        // max_lines cut text off
    ui_line *lines;
    uint32_t line_count, line_capacity;
    ui_layout_run *runs;
    uint32_t run_count, run_capacity;
    ui_layout_glyph *glyphs;
    uint32_t glyph_count, glyph_capacity;
} ui_paragraph_layout;

void ui_paragraph_layout_free(ui_paragraph_layout *l);

// Shared state for laying out text on one thread (the core): fonts, shaper and scratch.
typedef struct ui_text_context {
    ui_font_registry *fonts;
    ui_text_shaper shaper;
    float *advances;       // per code point
    uint32_t advance_capacity;
    uint8_t *line_levels;  // per code point of a line, then per piece
    uint32_t level_capacity;
    struct ui_piece *pieces;
    uint32_t piece_capacity;
    uint32_t *order;
    uint32_t order_capacity;
} ui_text_context;

ui_result ui_text_context_init(ui_text_context *c, ui_font_registry *fonts);
void ui_text_context_free(ui_text_context *c);

// A paragraph with its input and cached results. Results are recomputed only when what
// they depend on changes: shaping on text, spans, direction or scale; line layout also on
// width or max lines.
typedef struct ui_paragraph {
    uint8_t *text;
    uint32_t len;
    ui_text_style base;
    ui_text_span *spans;
    uint32_t span_count;
    char *families;
    uint32_t *family_offsets;
    uint32_t family_count;
    ui_bidi_dir dir;
    uint32_t max_lines;    // 0 = unlimited
    uint8_t *ellipsis;     // UTF-8; empty = cut without an ellipsis
    uint32_t ellipsis_len;

    ui_shaped_paragraph shaped;
    bool shaped_valid;
    ui_paragraph_layout layout;
    bool layout_valid;
    // How often each level was recomputed (tests, devtools).
    uint64_t shape_count, layout_count;
} ui_paragraph;

typedef struct ui_paragraph_desc {
    const uint8_t *text;   // valid UTF-8
    uint32_t len;
    ui_text_style base;
    const ui_text_span *spans; // validated: sorted, non-overlapping, on code point boundaries
    uint32_t span_count;
    const char *families;
    const uint32_t *family_offsets;
    uint32_t family_count;
    ui_bidi_dir dir;
    uint32_t max_lines;
    const uint8_t *ellipsis;
    uint32_t ellipsis_len;
} ui_paragraph_desc;

// Copies the description. Fails only on allocation (the paragraph keeps its old content).
ui_result ui_paragraph_set(ui_paragraph *p, const ui_paragraph_desc *desc);
void ui_paragraph_free(ui_paragraph *p);

// The layout at `scale` for `max_width` physical px (INFINITY or negative: unbounded).
// NULL on failure with *result set (allocation, or UI_ERR_WRONG_STATE without fonts).
const ui_paragraph_layout *ui_paragraph_layout_get(ui_paragraph *p, ui_text_context *c,
                                                   float scale, float max_width,
                                                   ui_result *result);
