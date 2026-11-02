// The bidi algorithm on arbitrary code points: levels stay in range, the paragraph level is
// 0 or 1, L1 resets trailing whitespace, and L2 produces a permutation.
#include <stdlib.h>
#include <string.h>

#include "fuzz_cps.h"
#include "unicode/bidi.h"
#include "unicode/ucd.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!size) return 0;
    static uint32_t cps[4096], visual[4096];
    static uint8_t levels[4096], seen[4096];
    ui_bidi_dir dir = (ui_bidi_dir)(data[0] % 3);
    size_t n = fuzz_cps(data + 1, size - 1, cps, 4096);
    ui_bidi b = {0};
    if (ui_bidi_resolve(&b, cps, n, dir) != UI_OK) abort();
    for (size_t i = 0; i < n; i++) {
        if (b.para_level[i] > 1) abort();
        if (dir == UI_BIDI_LTR && b.para_level[i] != 0) abort();
        if (dir == UI_BIDI_RTL && b.para_level[i] != 1) abort();
        if (b.levels[i] > UI_BIDI_MAX_DEPTH + 1) abort();
    }
    ui_bidi_line_levels(&b, 0, (uint32_t)n, levels);
    if (n && ui_ucd(cps[n - 1])->bidi == UI_BC_WS && levels[n - 1] != b.para_level[n - 1]) abort();
    ui_bidi_reorder(levels, (uint32_t)n, visual);
    memset(seen, 0, n);
    for (size_t i = 0; i < n; i++) {
        if (visual[i] >= n || seen[visual[i]]) abort();
        seen[visual[i]] = 1;
    }
    ui_bidi_free(&b);
    return 0;
}
