// Hit testing over a laid-out paragraph, for editing: point to text offset, offset to caret
// rectangle, and range to selection rectangles. Offsets are UTF-8 byte offsets into the
// paragraph's (sanitized) text and always fall on grapheme cluster boundaries. Coordinates
// are physical px relative to the paragraph origin, in the paragraph's last layout.
//
// Data model: each line holds its grapheme clusters in visual order, each with its byte
// range, horizontal extent and direction. A ligature covering several graphemes is split
// into one cluster per grapheme, at the font's GDEF ligature carets when it has them,
// evenly otherwise. Hard line breaks are not clusters: a caret never sits after one on the
// same line.
//
// Affinity disambiguates an offset that has two visual positions:
//  - at a soft line wrap, the end of one line and the start of the next;
//  - at a direction change, the edge of the run before it and of the run after it.
// UPSTREAM attaches the caret to the character before the offset, DOWNSTREAM to the one
// after it.
#pragma once

#include <stdint.h>

#include "text/paragraph.h"

typedef enum ui_affinity {
    UI_AFFINITY_DOWNSTREAM = 0,
    UI_AFFINITY_UPSTREAM = 1,
} ui_affinity;

typedef struct ui_text_position {
    uint32_t offset;
    ui_affinity affinity;
} ui_text_position;

typedef struct ui_text_rect {
    float x, y, width, height;
} ui_text_rect;

typedef struct ui_hit_cluster {
    uint32_t start, end; // bytes, one grapheme cluster
    float x0, x1;        // visual extent
    uint32_t line;
    bool rtl;
} ui_hit_cluster;

// Builds (or reuses) the hit data of the paragraph's current layout. Fails with
// UI_ERR_WRONG_STATE when the paragraph has no valid layout.
ui_result ui_text_hit_prepare(ui_paragraph *p, ui_text_context *c);

// The caret position nearest to (x, y): the line by y (clamped to the first and last), then
// the cluster by x (clamped to the line ends). The leading half of a cluster (left for LTR,
// right for RTL) gives its start with DOWNSTREAM affinity, the trailing half its end with
// UPSTREAM affinity.
ui_result ui_text_hit_point(ui_paragraph *p, ui_text_context *c, float x, float y,
                            ui_text_position *out);

// The caret for a position: `caret_width` px wide, centered on the boundary, spanning the
// line box. Offsets inside a cluster snap to its start.
ui_result ui_text_caret_rect(ui_paragraph *p, ui_text_context *c, ui_text_position pos,
                             float caret_width, ui_text_rect *out);

// Rectangles covering [start, end): per line, the clusters in the range, merged where they
// touch in visual order, so bidi text can give several per line. Writes at most `cap` and
// sets *count to how many there are.
ui_result ui_text_selection_rects(ui_paragraph *p, ui_text_context *c, uint32_t start,
                                  uint32_t end, ui_text_rect *rects, uint32_t cap,
                                  uint32_t *count);
