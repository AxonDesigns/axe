// Paragraph layout: line breaking (UAX #14 opportunities, emergency breaks), per-line bidi
// reordering (L1, L2), metrics with whole-pixel baselines, max lines with an ellipsis, and a
// two-level cache. All lengths are physical pixels; text offsets are UTF-8 byte offsets.
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "text/shape.h"

typedef enum ui_align {
    UI_ALIGN_START = 0, // left in LTR paragraphs, right in RTL ones
    UI_ALIGN_END,
    UI_ALIGN_LEFT,
    UI_ALIGN_RIGHT,
    UI_ALIGN_CENTER,
    UI_ALIGN_JUSTIFY,   // spaces stretch to fill the line; last lines start-aligned
} ui_align;

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
    ui_font *sized_font;   // the instance `font` names (owned by the registry)
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
    // Shaping older than this font registry generation is stale (a font was loaded since)
    // and is redone when laid out.
    uint64_t generation;
    uint64_t shapes;       // paragraphs shaped by this context (on its thread), for stats
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

typedef struct ui_paragraph_desc ui_paragraph_desc;

// A paragraph's input, immutable once created and shared by reference counting: the
// paragraph that shows it and the worker jobs that prepare it each hold a reference.
typedef struct ui_text_source {
    _Atomic uint32_t refs;
    uint8_t *text;
    uint32_t len;
    ui_text_style base;
    ui_text_span *spans;
    uint32_t span_count;
    char *families;
    uint32_t *family_offsets;
    uint32_t family_count;
    ui_bidi_dir dir;
    uint32_t max_lines;
    uint8_t *ellipsis;
    uint32_t ellipsis_len;
    ui_align align;
    float line_height;
} ui_text_source;

// Copies the description into a new source with one reference.
ui_result ui_text_source_create(const ui_paragraph_desc *desc, ui_text_source **out);
ui_text_source *ui_text_source_retain(ui_text_source *s);
void ui_text_source_release(ui_text_source *s); // thread: any
// What shaping reads from a source.
ui_text_input ui_text_source_input(const ui_text_source *s);

// A paragraph with its input and cached results. Results are recomputed only when what
// they depend on changes: shaping on text, spans, direction, scale or the font registry's
// generation; line layout also on width, max lines, alignment or line height.
typedef struct ui_paragraph {
    ui_text_source *source; // the input; the fields below point into it
    uint8_t *text;
    uint32_t len;
    ui_text_style base;
    ui_text_span *spans;
    uint32_t span_count;
    char *families;
    uint32_t *family_offsets;
    uint32_t family_count;
    ui_bidi_dir dir;
    uint8_t *ellipsis;     // UTF-8; empty = cut without an ellipsis
    uint32_t ellipsis_len;
    // Line layout parameters: the source's, unless styles override them.
    uint32_t max_lines;    // 0 = unlimited
    ui_align align;
    float line_height;     // multiple of the base size; 0 = from the fonts' metrics

    ui_shaped_paragraph shaped;
    bool shaped_valid;
    ui_paragraph_layout layout;
    bool layout_valid;
    // Hit-testing data of the current layout (text/hittest.h), built on first use.
    struct ui_hit_cluster *clusters;
    uint32_t cluster_count, cluster_capacity;
    uint32_t *line_clusters;  // first cluster of each line, line_count + 1 entries
    uint32_t line_cluster_capacity;
    bool hits_valid;
    // How often each level was recomputed (tests, devtools).
    uint64_t shape_count, layout_count;
} ui_paragraph;

struct ui_paragraph_desc {
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
    ui_align align;
    // 0: each line is as tall as its fonts need. Otherwise every line is line_height ×
    // base size, with the baseline placed by the base font's metrics, so fallback fonts with
    // tall metrics no longer change the spacing.
    float line_height;
};

// Copies the description. Fails only on allocation (the paragraph keeps its old content).
ui_result ui_paragraph_set(ui_paragraph *p, const ui_paragraph_desc *desc);
// Shows `source` (the paragraph takes a reference); cached results are dropped.
void ui_paragraph_set_source(ui_paragraph *p, ui_text_source *source);
// Installs shaping prepared elsewhere for the current source, taking over its arrays (*shaped
// is left empty). It is used when its scale is asked for and its generation is current.
void ui_paragraph_adopt_shaped(ui_paragraph *p, ui_shaped_paragraph *shaped);
// Sets the line layout parameters (styles may override the source's). The line layout is
// redone only if one changed.
void ui_paragraph_set_params(ui_paragraph *p, uint32_t max_lines, ui_align align,
                             float line_height);
void ui_paragraph_free(ui_paragraph *p);

// The layout at `scale` for `max_width` physical px (INFINITY or negative: unbounded).
// NULL on failure with *result set (allocation, or UI_ERR_WRONG_STATE without fonts).
const ui_paragraph_layout *ui_paragraph_layout_get(ui_paragraph *p, ui_text_context *c,
                                                   float scale, float max_width,
                                                   ui_result *result);
