// Font faces, sized font instances, shaping and glyph outlines. This module is the only code
// that includes hb.h; nothing HarfBuzz-specific crosses this header, so replacing HarfBuzz
// touches one directory. Thread: faces and fonts are immutable once created, so any number
// of threads may use them at once (HarfBuzz objects are made immutable); a ui_shaper is used
// by one thread at a time.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui/result.h"

typedef struct ui_face ui_face; // one face of a font file (immutable after loading)
typedef struct ui_font ui_font; // a face at one physical size and one variation instance

#define UI_TAG(a, b, c, d) \
    ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define UI_AXIS_WGHT UI_TAG('w', 'g', 'h', 't')
#define UI_AXIS_WDTH UI_TAG('w', 'd', 't', 'h')
#define UI_AXIS_ITAL UI_TAG('i', 't', 'a', 'l')
#define UI_AXIS_SLNT UI_TAG('s', 'l', 'n', 't')

typedef struct ui_face_info {
    char family[96];       // typographic family (name ID 16), else name ID 1; UTF-8
    uint16_t weight;       // default instance, 1..1000
    bool italic;           // default instance is italic or oblique
    bool has_wght;         // variable weight axis, with its range
    float wght_min, wght_max;
    bool has_ital, has_slnt;
    float slnt_min;        // most slanted value of the slnt axis (negative = clockwise)
    uint32_t upem;
} ui_face_info;

// Loads face `index` of a font file (TTF, OTF, TTC). UI_ERR_IO for unreadable files,
// UI_ERR_INVALID_ARGUMENT for data that is not a font.
ui_result ui_face_load_file(const char *path, uint32_t index, ui_face **out);
// Same from memory; the bytes are copied.
ui_result ui_face_load_memory(const void *data, size_t size, uint32_t index, ui_face **out);
void ui_face_destroy(ui_face *face);
const ui_face_info *ui_face_get_info(const ui_face *face);
// True if the face's cmap maps `cp` to a glyph.
bool ui_face_has_codepoint(const ui_face *face, uint32_t cp);

typedef struct ui_font_axis {
    uint32_t tag;
    float value;
} ui_font_axis;

// Synthesized style for faces that lack it (CSS font-synthesis): emboldening in em units
// (advances grow with it) and a horizontal slant (shear, x per unit of y).
typedef struct ui_font_synthesis {
    float embolden;
    float slant;
} ui_font_synthesis;

#define UI_SYNTHETIC_EMBOLDEN 0.025f
#define UI_SYNTHETIC_SLANT 0.2f // about 11 degrees

// Creates an instance at `size_px` physical pixels per em with the given axis values
// (unknown axes are ignored; values are clamped by HarfBuzz) and optional synthesis (NULL:
// none). Outlines, advances and extents all include the synthesis. The face must outlive
// it.
ui_result ui_font_create(ui_face *face, float size_px, const ui_font_axis *axes,
                         uint32_t axis_count, const ui_font_synthesis *synth, ui_font **out);
void ui_font_destroy(ui_font *font);
ui_face *ui_font_face(const ui_font *font);
float ui_font_size(const ui_font *font);

typedef struct ui_font_metrics {
    float ascent;   // px above the baseline
    float descent;  // px below the baseline (positive)
    float line_gap; // px
} ui_font_metrics;
void ui_font_get_metrics(const ui_font *font, ui_font_metrics *out);

// Glyph for a code point, or 0 (.notdef) when the face has none.
uint32_t ui_font_glyph(const ui_font *font, uint32_t cp);

// GDEF ligature caret positions within `glyph` (px from the glyph origin, in logical order
// for the direction). Returns how many carets exist; writes at most `cap`.
uint32_t ui_font_ligature_carets(const ui_font *font, uint32_t glyph, bool rtl, float *carets,
                                 uint32_t cap);

// ---- Shaping ---------------------------------------------------------------------------

// Glyph ID for characters that draw nothing: controls (tab, line feed, ...) and the line
// and paragraph separators. Fonts often map them to .notdef, which would show a box.
#define UI_GLYPH_INVISIBLE UINT32_MAX

typedef struct ui_glyph_pos {
    uint32_t glyph;
    uint32_t cluster;      // byte offset into the shaped text of the glyph's first character
    int32_t advance;       // 1/64 px
    int32_t x_offset;      // 1/64 px
    int32_t y_offset;      // 1/64 px, positive up
    bool unsafe_to_break;  // breaking the run before this glyph needs reshaping
} ui_glyph_pos;

// Reusable shaping state (a HarfBuzz buffer and the output array).
typedef struct ui_shaper ui_shaper;
ui_result ui_shaper_create(ui_shaper **out);
void ui_shaper_destroy(ui_shaper *shaper);

typedef struct ui_shape_request {
    const uint8_t *text;   // the whole paragraph, valid UTF-8 (context for shaping)
    uint32_t text_len;
    uint32_t start, len;   // the run to shape, in bytes
    uint32_t script;       // ISO 15924 tag ('Latn')
    bool rtl;
    const char *language;  // BCP 47 tag, not NUL-terminated; NULL: HarfBuzz's default
    uint32_t language_len;
} ui_shape_request;

// Shapes one run. Glyphs come out in visual order (reversed for RTL), with clusters as byte
// offsets into `text`. Controls and line/paragraph separators come out as
// UI_GLYPH_INVISIBLE with no advance, except a tab, whose advance is the font's space width
// (layout turns it into a tab stop). *glyphs stays valid until the next call on this shaper.
ui_result ui_shape(ui_shaper *shaper, ui_font *font, const ui_shape_request *req,
                   const ui_glyph_pos **glyphs, uint32_t *count);

// ---- Outlines --------------------------------------------------------------------------

// Receives a glyph outline in px, y up, relative to the glyph origin.
typedef struct ui_path_sink {
    void (*move_to)(void *user, float x, float y);
    void (*line_to)(void *user, float x, float y);
    void (*quad_to)(void *user, float cx, float cy, float x, float y);
    void (*cubic_to)(void *user, float c1x, float c1y, float c2x, float c2y, float x, float y);
    void (*close)(void *user);
    void *user;
} ui_path_sink;

// Draws a glyph's outline (at the font's variation instance). False if it has none, such
// as a space.
bool ui_font_draw_glyph(ui_font *font, uint32_t glyph, const ui_path_sink *sink);

// Ink bounds in px, y up. False for empty glyphs.
bool ui_font_glyph_bounds(const ui_font *font, uint32_t glyph, float *x0, float *y0,
                          float *x1, float *y1);
