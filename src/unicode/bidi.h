// The Unicode Bidirectional Algorithm (UAX #9) over code point arrays: paragraph levels
// (P1-P3), explicit levels (X1-X10), weak and neutral types (W1-W7, N0-N2), implicit
// levels (I1-I2), and per line the L1 level reset and the L2 visual order. No tailoring.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui/result.h"

#define UI_BIDI_MAX_DEPTH 125

typedef enum ui_bidi_dir {
    UI_BIDI_AUTO = 0, // each paragraph takes its first strong character's direction (P2, P3)
    UI_BIDI_LTR,
    UI_BIDI_RTL,
} ui_bidi_dir;

// Result and scratch space; zero-initialize, reuse across calls, free with ui_bidi_free.
typedef struct ui_bidi {
    uint32_t count;      // code points resolved by the last call
    uint8_t *classes;    // original Bidi_Class of each code point
    uint8_t *levels;     // resolved embedding level of each code point
    uint8_t *para_level; // level of the paragraph each code point belongs to
    // Scratch.
    uint8_t *types;
    uint32_t *match;     // matching PDI of an isolate initiator, or UINT32_MAX
    uint32_t *seq;       // indices of one isolating run sequence; also a stack
    uint32_t *run_of;    // level run of each kept code point
    uint32_t *runs;      // level runs: index of each run's first code point
    struct ui_bidi_pair *pairs; // bracket pairs of one sequence (at most half its length)
    uint32_t capacity;
} ui_bidi;

void ui_bidi_free(ui_bidi *b);

// Resolves levels for the text. Paragraphs end after each paragraph separator (P1); with
// UI_BIDI_AUTO each one picks its own direction. Fails only on allocation.
ui_result ui_bidi_resolve(ui_bidi *b, const uint32_t *cps, size_t n, ui_bidi_dir dir);

// True for characters removed by X9 (embedding controls, PDF, BN). They keep the level of
// the preceding character so they render with it, but are not part of the algorithm.
bool ui_bidi_removed(uint8_t bidi_class);

// L1: copies the levels of the line [start, end) into out and resets segment separators,
// paragraph separators, and trailing whitespace and isolate controls (also before
// separators) to the paragraph level.
void ui_bidi_line_levels(const ui_bidi *b, uint32_t start, uint32_t end, uint8_t *out);

// L2: visual[k] is the index (0-based within the line) of the code point shown k-th from
// the left, given the line's levels after L1.
void ui_bidi_reorder(const uint8_t *levels, uint32_t n, uint32_t *visual);
