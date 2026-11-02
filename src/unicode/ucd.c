#include "unicode/ucd.h"

bool ui_ucd_paired_bracket(uint32_t cp, uint32_t *pair, bool *open) {
    uint32_t lo = 0, hi = ui_ucd_bracket_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (ui_ucd_brackets[mid].cp < cp) lo = mid + 1;
        else hi = mid;
    }
    if (lo == ui_ucd_bracket_count || ui_ucd_brackets[lo].cp != cp) return false;
    *pair = ui_ucd_brackets[lo].pair;
    *open = ui_ucd_brackets[lo].open;
    return true;
}

const uint8_t *ui_ucd_script_extensions(const ui_ucd_record *r, uint32_t *count) {
    if (!r->scx) {
        *count = 1;
        return &r->script;
    }
    const ui_ucd_scx_set *set = &ui_ucd_scx_sets[r->scx];
    *count = set->count;
    return &ui_ucd_scx_scripts[set->offset];
}
