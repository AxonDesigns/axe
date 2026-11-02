// Line breaking and grapheme clusters on arbitrary code points: structural invariants.
#include <stdlib.h>

#include "fuzz_cps.h"
#include "unicode/segment.h"
#include "unicode/ucd.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static uint32_t cps[4096];
    static uint8_t lb[4097], gb[4097];
    size_t n = fuzz_cps(data, size, cps, 4096);
    ui_line_breaker breaker = {0};
    if (ui_line_breaks(&breaker, cps, n, lb) != UI_OK) abort();
    ui_line_breaker_free(&breaker);
    ui_grapheme_breaks(cps, n, gb);
    if (lb[n] != UI_LINE_BREAK_MANDATORY) abort(); // LB3
    if (n && lb[0] != UI_LINE_BREAK_NONE) abort();  // LB2
    for (size_t i = 1; i < n; i++) {
        uint8_t prev = ui_ucd(cps[i - 1])->lb, cur = ui_ucd(cps[i])->lb;
        // LB4/LB5: always break after hard breaks, except inside CR LF.
        bool hard = prev == UI_LB_BK || prev == UI_LB_LF || prev == UI_LB_NL ||
                    (prev == UI_LB_CR && cur != UI_LB_LF);
        if (hard != (lb[i] == UI_LINE_BREAK_MANDATORY)) abort();
        // LB7: never break before a space (hard breaks come first: LB4 and LB5 win).
        if (cur == UI_LB_SP && !hard && lb[i] != UI_LINE_BREAK_NONE) abort();
        if (lb[i] > UI_LINE_BREAK_MANDATORY || gb[i] > 1) abort();
    }
    if (n && (!gb[0] || !gb[n])) abort(); // GB1, GB2
    return 0;
}
