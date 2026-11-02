// UAX #9 revision 52 (Unicode 18.0). Characters removed by X9 stay in the arrays and are
// skipped: level runs and isolating run sequences are lists of kept indices, so every rule
// that looks at "the previous character" sees through them, as X9 requires.
#include "unicode/bidi.h"

#include <stdlib.h>
#include <string.h>

#include "runtime/common.h"
#include "unicode/ucd.h"

#define NONE UINT32_MAX
#define BRACKET_STACK 63 // BD16

void ui_bidi_free(ui_bidi *b) {
    free(b->classes);
    free(b->levels);
    free(b->para_level);
    free(b->types);
    free(b->match);
    free(b->seq);
    free(b->run_of);
    free(b->runs);
    free(b->pairs);
    memset(b, 0, sizeof *b);
}

static ui_result reserve(ui_bidi *b, uint32_t n) {
    if (n <= b->capacity) return UI_OK;
    uint32_t cap = b->capacity ? b->capacity : 64;
    while (cap < n) cap = cap > UINT32_MAX / 2 ? n : cap * 2;
    void *p[9] = {
        realloc(b->classes, cap), realloc(b->levels, cap), realloc(b->para_level, cap),
        realloc(b->types, cap),
    };
    if (p[0]) b->classes = p[0];
    if (p[1]) b->levels = p[1];
    if (p[2]) b->para_level = p[2];
    if (p[3]) b->types = p[3];
    p[4] = realloc(b->match, (size_t)cap * 4);
    if (p[4]) b->match = p[4];
    p[5] = realloc(b->seq, (size_t)cap * 4);
    if (p[5]) b->seq = p[5];
    p[6] = realloc(b->run_of, (size_t)cap * 4);
    if (p[6]) b->run_of = p[6];
    p[7] = realloc(b->runs, (size_t)cap * 4);
    if (p[7]) b->runs = p[7];
    p[8] = realloc(b->pairs, (size_t)(cap / 2 + 1) * 8); // two uint32_t per pair
    if (p[8]) b->pairs = p[8];
    for (int i = 0; i < 9; i++) {
        if (!p[i]) return UI_ERR_OUT_OF_MEMORY; // grown arrays are kept; capacity unchanged
    }
    b->capacity = cap;
    return UI_OK;
}

bool ui_bidi_removed(uint8_t c) {
    return c == UI_BC_RLE || c == UI_BC_LRE || c == UI_BC_RLO || c == UI_BC_LRO ||
           c == UI_BC_PDF || c == UI_BC_BN;
}

static bool is_isolate_initiator(uint8_t c) {
    return c == UI_BC_LRI || c == UI_BC_RLI || c == UI_BC_FSI;
}

static bool is_neutral_or_isolate(uint8_t t) {
    return t == UI_BC_B || t == UI_BC_S || t == UI_BC_WS || t == UI_BC_ON ||
           t == UI_BC_LRI || t == UI_BC_RLI || t == UI_BC_FSI || t == UI_BC_PDI;
}

// Strong direction for N0/N1: L, or R for R, EN and AN; NONE otherwise.
static uint8_t strong_dir(uint8_t t) {
    if (t == UI_BC_L) return UI_BC_L;
    if (t == UI_BC_R || t == UI_BC_EN || t == UI_BC_AN || t == UI_BC_AL) return UI_BC_R;
    return 0xFF;
}

// P2/P3 over [from, to): 0 for L or no strong character, 1 for R or AL. Skips the content
// of isolates (to the matching PDI, or the end when unmatched).
static uint8_t first_strong_level(const ui_bidi *b, uint32_t from, uint32_t to) {
    for (uint32_t i = from; i < to; i++) {
        uint8_t c = b->classes[i];
        if (c == UI_BC_L) return 0;
        if (c == UI_BC_R || c == UI_BC_AL) return 1;
        if (is_isolate_initiator(c)) {
            if (b->match[i] == NONE) return 0;
            i = b->match[i];
        }
    }
    return 0;
}

// Normalizes the canonically equivalent angle brackets (BD16 compares canonical
// equivalents): U+2329/U+232A decompose to U+3008/U+3009.
static uint32_t canonical_bracket(uint32_t cp) {
    if (cp == 0x2329) return 0x3008;
    if (cp == 0x232A) return 0x3009;
    return cp;
}

typedef struct ui_bidi_pair {
    uint32_t open, close; // positions in the sequence
} bracket_pair;

static int pair_order(const void *x, const void *y) {
    const bracket_pair *a = x, *b = y;
    return a->open < b->open ? -1 : a->open > b->open;
}

static void set_bracket(ui_bidi *b, const uint32_t *s, uint32_t m, uint32_t k, uint8_t dir) {
    b->types[s[k]] = dir;
    // NSMs that followed the bracket (W1 made them ON) take its new type.
    for (uint32_t j = k + 1; j < m && b->classes[s[j]] == UI_BC_NSM; j++) b->types[s[j]] = dir;
}

// N0 over one isolating run sequence.
static void resolve_brackets(ui_bidi *b, const uint32_t *cps, const uint32_t *s, uint32_t m,
                             uint8_t sos, uint8_t level) {
    struct {
        uint32_t close_cp, pos;
    } stack[BRACKET_STACK];
    uint32_t depth = 0;
    bracket_pair *pairs = b->pairs; // each pair consumes two positions: at most m / 2
    uint32_t npairs = 0;
    for (uint32_t k = 0; k < m; k++) {
        if (b->types[s[k]] != UI_BC_ON) continue;
        uint32_t pair;
        bool open;
        uint32_t cp = cps[s[k]];
        if (!ui_ucd_paired_bracket(cp, &pair, &open)) continue;
        if (open) {
            if (depth == BRACKET_STACK) break; // BD16: stop at stack overflow
            stack[depth].close_cp = canonical_bracket(pair);
            stack[depth].pos = k;
            depth++;
        } else {
            uint32_t c = canonical_bracket(cp);
            for (uint32_t j = depth; j-- > 0;) {
                if (stack[j].close_cp == c) {
                    pairs[npairs++] = (bracket_pair){stack[j].pos, k};
                    depth = j;
                    break;
                }
            }
        }
    }
    if (!npairs) return;
    qsort(pairs, npairs, sizeof *pairs, pair_order);
    uint8_t e = (level & 1) ? UI_BC_R : UI_BC_L;
    for (uint32_t p = 0; p < npairs; p++) {
        bool found_e = false, found_opp = false;
        for (uint32_t k = pairs[p].open + 1; k < pairs[p].close; k++) {
            uint8_t d = strong_dir(b->types[s[k]]);
            if (d == e) {
                found_e = true;
                break;
            }
            if (d != 0xFF) found_opp = true;
        }
        uint8_t dir;
        if (found_e) {
            dir = e;
        } else if (found_opp) {
            uint8_t ctx = sos;
            for (uint32_t k = pairs[p].open; k-- > 0;) {
                uint8_t d = strong_dir(b->types[s[k]]);
                if (d != 0xFF) {
                    ctx = d;
                    break;
                }
            }
            dir = ctx != e ? ctx : e;
        } else {
            continue;
        }
        set_bracket(b, s, m, pairs[p].open, dir);
        set_bracket(b, s, m, pairs[p].close, dir);
    }
}

// W1-W7 and N0-N2 over one isolating run sequence s[0..m). Only types change here: sos and
// eos of later sequences must see the explicit levels, so I1-I2 run once at the end.
static void resolve_sequence(ui_bidi *b, const uint32_t *cps, const uint32_t *s, uint32_t m,
                             uint32_t ps, uint32_t pe, uint8_t para) {
    uint8_t *t = b->types;
    uint8_t level = b->levels[s[0]];
    uint8_t prev_level = para, next_level = para;
    for (uint32_t i = s[0]; i-- > ps;) {
        if (!ui_bidi_removed(b->classes[i])) {
            prev_level = b->levels[i];
            break;
        }
    }
    uint32_t last = s[m - 1];
    if (!is_isolate_initiator(b->classes[last])) {
        for (uint32_t i = last + 1; i < pe; i++) {
            if (!ui_bidi_removed(b->classes[i])) {
                next_level = b->levels[i];
                break;
            }
        }
    }
    uint8_t sos = ((level > prev_level ? level : prev_level) & 1) ? UI_BC_R : UI_BC_L;
    uint8_t eos = ((level > next_level ? level : next_level) & 1) ? UI_BC_R : UI_BC_L;

    // W1
    for (uint32_t k = 0; k < m; k++) {
        if (t[s[k]] != UI_BC_NSM) continue;
        if (k == 0) t[s[k]] = sos;
        else {
            uint8_t p = t[s[k - 1]];
            t[s[k]] = is_isolate_initiator(p) || p == UI_BC_PDI ? UI_BC_ON : p;
        }
    }
    // W2, W3
    uint8_t strong = sos;
    for (uint32_t k = 0; k < m; k++) {
        uint8_t c = t[s[k]];
        if (c == UI_BC_L || c == UI_BC_R || c == UI_BC_AL) strong = c;
        else if (c == UI_BC_EN && strong == UI_BC_AL) t[s[k]] = UI_BC_AN;
    }
    for (uint32_t k = 0; k < m; k++) {
        if (t[s[k]] == UI_BC_AL) t[s[k]] = UI_BC_R;
    }
    // W4
    for (uint32_t k = 1; k + 1 < m; k++) {
        uint8_t c = t[s[k]], p = t[s[k - 1]], nx = t[s[k + 1]];
        if (c == UI_BC_ES && p == UI_BC_EN && nx == UI_BC_EN) t[s[k]] = UI_BC_EN;
        else if (c == UI_BC_CS && p == nx && (p == UI_BC_EN || p == UI_BC_AN)) t[s[k]] = p;
    }
    // W5
    for (uint32_t k = 0; k < m;) {
        if (t[s[k]] != UI_BC_ET) {
            k++;
            continue;
        }
        uint32_t end = k;
        while (end < m && t[s[end]] == UI_BC_ET) end++;
        if ((k > 0 && t[s[k - 1]] == UI_BC_EN) || (end < m && t[s[end]] == UI_BC_EN)) {
            for (uint32_t j = k; j < end; j++) t[s[j]] = UI_BC_EN;
        }
        k = end;
    }
    // W6
    for (uint32_t k = 0; k < m; k++) {
        uint8_t c = t[s[k]];
        if (c == UI_BC_ES || c == UI_BC_ET || c == UI_BC_CS) t[s[k]] = UI_BC_ON;
    }
    // W7
    strong = sos;
    for (uint32_t k = 0; k < m; k++) {
        uint8_t c = t[s[k]];
        if (c == UI_BC_L || c == UI_BC_R) strong = c;
        else if (c == UI_BC_EN && strong == UI_BC_L) t[s[k]] = UI_BC_L;
    }
    // N0
    resolve_brackets(b, cps, s, m, sos, level);
    // N1, N2
    uint8_t e = (level & 1) ? UI_BC_R : UI_BC_L;
    for (uint32_t k = 0; k < m;) {
        if (!is_neutral_or_isolate(t[s[k]])) {
            k++;
            continue;
        }
        uint32_t end = k;
        while (end < m && is_neutral_or_isolate(t[s[end]])) end++;
        uint8_t before = k == 0 ? sos : strong_dir(t[s[k - 1]]);
        uint8_t after = end == m ? eos : strong_dir(t[s[end]]);
        uint8_t dir = before == after ? before : e;
        for (uint32_t j = k; j < end; j++) t[s[j]] = dir;
        k = end;
    }
}

typedef struct embed_entry {
    uint8_t level;
    uint8_t override; // UI_BC_L, UI_BC_R, or 0xFF for none
    bool isolate;
} embed_entry;

static void resolve_paragraph(ui_bidi *b, const uint32_t *cps, uint32_t ps, uint32_t pe,
                              ui_bidi_dir dir) {
    uint8_t *cls = b->classes;
    // BD9: matching PDIs. match[] links both ways; seq[] is the initiator stack.
    uint32_t depth = 0;
    for (uint32_t i = ps; i < pe; i++) {
        b->match[i] = NONE;
        if (is_isolate_initiator(cls[i])) {
            b->seq[depth++] = i;
        } else if (cls[i] == UI_BC_PDI && depth) {
            uint32_t j = b->seq[--depth];
            b->match[j] = i;
            b->match[i] = j;
        }
    }
    uint8_t para = dir == UI_BIDI_LTR   ? 0
                   : dir == UI_BIDI_RTL ? 1
                                        : first_strong_level(b, ps, pe);

    // X1-X8.
    embed_entry stack[UI_BIDI_MAX_DEPTH + 2];
    uint32_t sp = 0;
    stack[sp++] = (embed_entry){para, 0xFF, false};
    uint32_t overflow_isolate = 0, overflow_embed = 0, valid_isolate = 0;
    for (uint32_t i = ps; i < pe; i++) {
        uint8_t c = cls[i];
        b->types[i] = c;
        embed_entry *top = &stack[sp - 1];
        switch (c) {
        case UI_BC_RLE:
        case UI_BC_LRE:
        case UI_BC_RLO:
        case UI_BC_LRO: {
            bool rtl = c == UI_BC_RLE || c == UI_BC_RLO;
            uint8_t level = rtl ? (uint8_t)((top->level + 1) | 1) : (uint8_t)((top->level + 2) & ~1);
            b->levels[i] = top->level;
            if (level <= UI_BIDI_MAX_DEPTH && !overflow_isolate && !overflow_embed) {
                uint8_t ov = c == UI_BC_RLO ? UI_BC_R : c == UI_BC_LRO ? UI_BC_L : 0xFF;
                stack[sp++] = (embed_entry){level, ov, false};
            } else if (!overflow_isolate) {
                overflow_embed++;
            }
            break;
        }
        case UI_BC_RLI:
        case UI_BC_LRI:
        case UI_BC_FSI: {
            b->levels[i] = top->level;
            if (top->override != 0xFF) b->types[i] = top->override;
            bool rtl = c == UI_BC_RLI;
            if (c == UI_BC_FSI)
                rtl = first_strong_level(b, i + 1, b->match[i] == NONE ? pe : b->match[i]) == 1;
            uint8_t level = rtl ? (uint8_t)((top->level + 1) | 1) : (uint8_t)((top->level + 2) & ~1);
            if (level <= UI_BIDI_MAX_DEPTH && !overflow_isolate && !overflow_embed) {
                valid_isolate++;
                stack[sp++] = (embed_entry){level, 0xFF, true};
            } else {
                overflow_isolate++;
            }
            break;
        }
        case UI_BC_PDI:
            if (overflow_isolate) {
                overflow_isolate--;
            } else if (valid_isolate) {
                overflow_embed = 0;
                while (!stack[sp - 1].isolate) sp--;
                sp--;
                valid_isolate--;
            }
            top = &stack[sp - 1];
            b->levels[i] = top->level;
            if (top->override != 0xFF) b->types[i] = top->override;
            break;
        case UI_BC_PDF:
            if (overflow_isolate) {
            } else if (overflow_embed) {
                overflow_embed--;
            } else if (!top->isolate && sp >= 2) {
                sp--;
            }
            b->levels[i] = stack[sp - 1].level;
            break;
        case UI_BC_B:
            b->levels[i] = para;
            break;
        case UI_BC_BN:
            b->levels[i] = top->level;
            break;
        default:
            b->levels[i] = top->level;
            if (top->override != 0xFF) b->types[i] = top->override;
            break;
        }
    }

    // X10: level runs over kept characters, then isolating run sequences.
    uint32_t nruns = 0;
    uint8_t run_level = 0;
    for (uint32_t i = ps; i < pe; i++) {
        if (ui_bidi_removed(cls[i])) continue;
        if (!nruns || b->levels[i] != run_level) {
            b->runs[nruns++] = i;
            run_level = b->levels[i];
        }
        b->run_of[i] = nruns - 1;
    }
    for (uint32_t r = 0; r < nruns; r++) {
        uint32_t first = b->runs[r];
        if (cls[first] == UI_BC_PDI && b->match[first] != NONE) continue; // continues a sequence
        uint32_t m = 0;
        uint32_t cur = r;
        for (;;) {
            uint32_t lastk = NONE;
            for (uint32_t i = b->runs[cur]; i < pe; i++) {
                if (ui_bidi_removed(cls[i])) continue;
                if (b->run_of[i] != cur) break;
                b->seq[m++] = i;
                lastk = i;
            }
            if (lastk == NONE || !is_isolate_initiator(cls[lastk]) || b->match[lastk] == NONE)
                break;
            cur = b->run_of[b->match[lastk]];
        }
        resolve_sequence(b, cps, b->seq, m, ps, pe, para);
    }

    // I1, I2. Removed characters take the level of the preceding character.
    for (uint32_t i = ps; i < pe; i++) {
        b->para_level[i] = para;
        uint8_t *lv = &b->levels[i];
        if (ui_bidi_removed(cls[i])) {
            *lv = i > ps ? b->levels[i - 1] : para;
            continue;
        }
        uint8_t c = b->types[i];
        if (!(*lv & 1)) {
            if (c == UI_BC_R) *lv += 1;
            else if (c == UI_BC_AN || c == UI_BC_EN) *lv += 2;
        } else if (c == UI_BC_L || c == UI_BC_EN || c == UI_BC_AN) {
            *lv += 1;
        }
    }
}

ui_result ui_bidi_resolve(ui_bidi *b, const uint32_t *cps, size_t n, ui_bidi_dir dir) {
    if (n >= UINT32_MAX) return UI_ERR_INVALID_ARGUMENT;
    ui_result res = reserve(b, (uint32_t)n + 1);
    if (res != UI_OK) return res;
    b->count = (uint32_t)n;
    for (uint32_t i = 0; i < n; i++) b->classes[i] = ui_ucd(cps[i])->bidi;
    uint32_t ps = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (b->classes[i] == UI_BC_B) {
            resolve_paragraph(b, cps, ps, i + 1, dir);
            ps = i + 1;
        }
    }
    if (ps < n) resolve_paragraph(b, cps, ps, (uint32_t)n, dir);
    return UI_OK;
}

void ui_bidi_line_levels(const ui_bidi *b, uint32_t start, uint32_t end, uint8_t *out) {
    bool trailing = true;
    for (uint32_t i = end; i-- > start;) {
        uint8_t c = b->classes[i];
        uint8_t para = b->para_level[i];
        if (c == UI_BC_S || c == UI_BC_B) {
            out[i - start] = para;
            trailing = true;
        } else if (trailing && (c == UI_BC_WS || is_isolate_initiator(c) || c == UI_BC_PDI ||
                                ui_bidi_removed(c))) {
            out[i - start] = para;
        } else {
            out[i - start] = b->levels[i];
            trailing = false;
        }
    }
}

void ui_bidi_reorder(const uint8_t *levels, uint32_t n, uint32_t *visual) {
    uint8_t max = 0, min_odd = 0xFF;
    for (uint32_t i = 0; i < n; i++) {
        visual[i] = i;
        if (levels[i] > max) max = levels[i];
        if ((levels[i] & 1) && levels[i] < min_odd) min_odd = levels[i];
    }
    if (min_odd == 0xFF) return;
    for (uint8_t level = max; level >= min_odd; level--) {
        for (uint32_t i = 0; i < n;) {
            if (levels[visual[i]] < level) {
                i++;
                continue;
            }
            uint32_t j = i;
            while (j < n && levels[visual[j]] >= level) j++;
            for (uint32_t a = i, z = j - 1; a < z; a++, z--) {
                uint32_t tmp = visual[a];
                visual[a] = visual[z];
                visual[z] = tmp;
            }
            i = j;
        }
        if (level == 0) break;
    }
}
