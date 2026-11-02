// Itemization and shaping: turns UTF-8 text with style spans into runs of positioned glyphs.
// The result depends on text, spans, direction and scale, but not on the layout width, so a
// width change (a resize, a spring) re-breaks lines without reshaping.
//
// Pipeline: decode -> bidi levels (UAX #9) -> script runs (UAX #24) -> grapheme clusters
// (UAX #29) -> font per cluster (family match + fallback) -> runs where level, script,
// font or shaping style change -> HarfBuzz per run. Color is not a shaping boundary.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "text/font_registry.h"
#include "unicode/bidi.h"
#include "unicode/segment.h"

#define UI_TEXT_DEFAULT_FAMILY UINT16_MAX

typedef struct ui_text_style {
    uint16_t family;  // index into the input's family lists, or UI_TEXT_DEFAULT_FAMILY
    uint16_t weight;  // 1..1000
    uint8_t style;    // ui_font_style
    float size;       // logical px
    float color[4];   // linear, straight alpha
} ui_text_style;

typedef struct ui_text_span {
    uint32_t start, end; // byte offsets; sorted, non-overlapping, on code point boundaries
    ui_text_style style;
} ui_text_span;

typedef struct ui_text_input {
    const uint8_t *text; // valid UTF-8
    uint32_t len;
    ui_text_style base;  // for bytes no span covers
    const ui_text_span *spans;
    uint32_t span_count;
    const char *families;          // family lists, concatenated
    const uint32_t *family_offsets; // family_count + 1 offsets into `families`
    uint32_t family_count;
    ui_bidi_dir dir;
} ui_text_input;

typedef struct ui_shaped_run {
    uint32_t start, end;             // byte range
    uint32_t glyph_start, glyph_count; // into ui_shaped_paragraph.glyphs, visual order
    ui_font_key font;
    uint32_t script;                 // ISO 15924 tag
    uint32_t style;                  // shaping style: 0 = base, else span index + 1
    uint8_t level;                   // bidi embedding level (odd = RTL)
    float ascent, descent;           // px
} ui_shaped_run;

typedef struct ui_shaped_paragraph {
    float scale;
    // Per code point; offsets has cp_count + 1 entries (the last is the text length).
    uint32_t cp_count;
    uint32_t *cps;
    uint32_t *offsets;
    uint8_t *line_breaks;  // ui_line_break before each code point (cp_count + 1)
    uint8_t *graphemes;    // 1 where a grapheme cluster starts (cp_count + 1)
    uint32_t *styles;      // style index per code point (0 = base, else span index + 1)
    ui_bidi bidi;          // levels, classes and paragraph levels per code point
    ui_shaped_run *runs;   // in logical order
    uint32_t run_count;
    ui_glyph_pos *glyphs;
    uint32_t glyph_count;
    uint32_t cp_capacity, run_capacity, glyph_capacity;
} ui_shaped_paragraph;

void ui_shaped_paragraph_free(ui_shaped_paragraph *p);

// Reusable state for shaping (HarfBuzz buffer, line breaker, scratch).
typedef struct ui_text_shaper {
    ui_shaper *hb;
    ui_line_breaker breaker;
    uint8_t *scripts;
    uint32_t *faces;
    uint32_t scratch_capacity;
    uint32_t *style_faces; // primary face per style (base + spans)
    uint32_t style_face_capacity;
} ui_text_shaper;

ui_result ui_text_shaper_init(ui_text_shaper *s);
void ui_text_shaper_free(ui_text_shaper *s);

// The style a style index refers to.
const ui_text_style *ui_text_input_style(const ui_text_input *in, uint32_t index);
// The family list text of a style (empty for the default family).
void ui_text_input_family(const ui_text_input *in, const ui_text_style *style,
                          const char **text, uint32_t *len);

// Shapes the paragraph at `scale` (font sizes become size * scale physical px). `out` is
// reused: its arrays grow as needed. Fails on allocation, or with UI_ERR_WRONG_STATE when
// there are no fonts at all.
ui_result ui_shape_paragraph(ui_text_shaper *s, ui_font_registry *fonts,
                             const ui_text_input *in, float scale, ui_shaped_paragraph *out);

// Shapes bytes [start, end) of run `run` again, for a line break inside it that HarfBuzz
// flagged unsafe. Glyphs are valid until the next call on the shaper.
ui_result ui_reshape_range(ui_text_shaper *s, ui_font_registry *fonts, const ui_text_input *in,
                           const ui_shaped_run *run, uint32_t start, uint32_t end,
                           const ui_glyph_pos **glyphs, uint32_t *count);

// Index of the code point that starts at or contains byte `offset`.
uint32_t ui_shaped_cp_at(const ui_shaped_paragraph *p, uint32_t offset);
