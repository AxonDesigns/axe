// Text segmentation over code point arrays: extended grapheme clusters (UAX #29) and line
// break opportunities (UAX #14). Both write one entry per position: position i is the
// boundary before cps[i], so arrays have n + 1 entries (position n is the end of text).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ui/result.h"

// out[i] = 1 where an extended grapheme cluster boundary lies before cps[i]. out[0] and
// out[n] are 1 for non-empty text (GB1, GB2).
void ui_grapheme_breaks(const uint32_t *cps, size_t n, uint8_t *out);

typedef enum ui_line_break {
    UI_LINE_BREAK_NONE = 0,  // no break allowed here
    UI_LINE_BREAK_ALLOWED,   // a break opportunity
    UI_LINE_BREAK_MANDATORY, // a hard line break (after BK, CR, LF, NL; and at end of text)
} ui_line_break;

// Reusable scratch space for line breaking; zero-initialize, free with
// ui_line_breaker_free.
typedef struct ui_line_breaker {
    struct ui_lb_unit *units;
    uint32_t capacity;
} ui_line_breaker;

void ui_line_breaker_free(ui_line_breaker *lb);

// Default UAX #14 line breaking, no tailoring. out[i] is a ui_line_break for the position
// before cps[i]; out[0] is NONE (LB2) and out[n] is MANDATORY (LB3).
// Fails only on allocation.
ui_result ui_line_breaks(ui_line_breaker *lb, const uint32_t *cps, size_t n, uint8_t *out);
