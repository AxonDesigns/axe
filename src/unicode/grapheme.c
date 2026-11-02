// Extended grapheme clusters, UAX #29 rules GB1-GB999 as of Unicode 18.0.
#include "unicode/segment.h"

#include "unicode/ucd.h"

static bool is_control(uint8_t g) {
    return g == UI_GCB_CONTROL || g == UI_GCB_CR || g == UI_GCB_LF;
}

void ui_grapheme_breaks(const uint32_t *cps, size_t n, uint8_t *out) {
    if (n == 0) {
        out[0] = 0;
        return;
    }
    out[0] = 1;
    out[n] = 1;
    const ui_ucd_record *prev = ui_ucd(cps[0]);
    // GB9c: the text before the position matches \p{InCB=Linker} \p{InCB=Extend}*.
    bool incb_linker = ui_ucd_incb(prev) == UI_INCB_LINKER;
    // GB11: \p{Extended_Pictographic} Extend* seen (pict), then ZWJ right after it (zwj).
    bool pict = prev->flags & UI_UCD_EXTPICT;
    bool pict_zwj = false;
    // GB12/13: regional indicators immediately before the position.
    uint32_t ri = prev->gcb == UI_GCB_REGIONAL_INDICATOR;

    for (size_t i = 1; i < n; i++) {
        const ui_ucd_record *cur = ui_ucd(cps[i]);
        uint8_t a = prev->gcb, b = cur->gcb;
        bool brk;
        if (a == UI_GCB_CR && b == UI_GCB_LF) brk = false;                      // GB3
        else if (is_control(a) || is_control(b)) brk = true;                    // GB4, GB5
        else if (a == UI_GCB_L && (b == UI_GCB_L || b == UI_GCB_V || b == UI_GCB_LV ||
                                   b == UI_GCB_LVT))
            brk = false;                                                        // GB6
        else if ((a == UI_GCB_LV || a == UI_GCB_V) && (b == UI_GCB_V || b == UI_GCB_T))
            brk = false;                                                        // GB7
        else if ((a == UI_GCB_LVT || a == UI_GCB_T) && b == UI_GCB_T) brk = false; // GB8
        else if (b == UI_GCB_EXTEND || b == UI_GCB_ZWJ) brk = false;            // GB9
        else if (b == UI_GCB_SPACINGMARK) brk = false;                          // GB9a
        else if (a == UI_GCB_PREPEND) brk = false;                              // GB9b
        else if (incb_linker && ui_ucd_incb(cur) == UI_INCB_CONSONANT) brk = false; // GB9c
        else if (pict_zwj && (cur->flags & UI_UCD_EXTPICT)) brk = false;        // GB11
        else if (a == UI_GCB_REGIONAL_INDICATOR && b == UI_GCB_REGIONAL_INDICATOR && (ri & 1))
            brk = false;                                                        // GB12, GB13
        else brk = true;                                                        // GB999
        out[i] = brk;

        // Update the left-context state with cur.
        ui_incb incb = ui_ucd_incb(cur);
        if (incb == UI_INCB_LINKER) incb_linker = true;
        else if (incb != UI_INCB_EXTEND) incb_linker = false;

        pict_zwj = pict && b == UI_GCB_ZWJ;
        if (cur->flags & UI_UCD_EXTPICT) pict = true;
        else if (b != UI_GCB_EXTEND) pict = false;

        ri = b == UI_GCB_REGIONAL_INDICATOR ? ri + 1 : 0;
        prev = cur;
    }
}
