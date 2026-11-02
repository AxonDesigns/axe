// Runs the official Unicode conformance files against our algorithms:
//   test_unicode_conformance bidi      tests/unicode/BidiCharacterTest.txt
//   test_unicode_conformance linebreak tests/unicode/LineBreakTest.txt
//   test_unicode_conformance grapheme  tests/unicode/GraphemeBreakTest.txt
// Every case must pass; deliberate deviations would be listed in docs/decisions.md and
// skipped here by line number (there are none).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unicode/bidi.h"
#include "unicode/segment.h"
#include "unicode/ucd.h"

#define MAX_CPS 4096
#define MAX_REPORTS 10

static const char *const BREAK = "\xC3\xB7";    // ÷
static const char *const NO_BREAK = "\xC3\x97"; // ×

typedef struct stats {
    unsigned cases, failures;
} stats;

static void report(stats *st, unsigned line, const char *what, const char *text) {
    st->failures++;
    if (st->failures <= MAX_REPORTS) fprintf(stderr, "line %u: %s: %s", line, what, text);
}

// Parses "÷ 0061 × 0308 ÷" into code points and expected boundaries (1 = break).
static bool parse_breaks(char *text, uint32_t *cps, size_t *n, uint8_t *expected) {
    *n = 0;
    size_t markers = 0;
    for (char *tok = strtok(text, " \t\r\n"); tok; tok = strtok(NULL, " \t\r\n")) {
        if (!strcmp(tok, BREAK) || !strcmp(tok, NO_BREAK)) {
            if (markers > MAX_CPS) return false;
            expected[markers++] = !strcmp(tok, BREAK);
        } else {
            if (*n >= MAX_CPS) return false;
            cps[(*n)++] = (uint32_t)strtoul(tok, NULL, 16);
        }
    }
    return markers == *n + 1;
}

static void run_segmentation(FILE *f, bool line_break, stats *st) {
    static char buf[1 << 16], copy[1 << 16];
    uint32_t cps[MAX_CPS];
    uint8_t expected[MAX_CPS + 1], got[MAX_CPS + 1];
    ui_line_breaker lb = {0};
    unsigned lineno = 0;
    while (fgets(buf, sizeof buf, f)) {
        lineno++;
        memcpy(copy, buf, sizeof buf);
        char *hash = strchr(buf, '#');
        if (hash) *hash = 0;
        size_t n;
        if (!strpbrk(buf, "0123456789ABCDEFabcdef")) continue;
        if (!parse_breaks(buf, cps, &n, expected)) {
            report(st, lineno, "unparsable", copy);
            continue;
        }
        st->cases++;
        if (line_break) {
            if (ui_line_breaks(&lb, cps, n, got) != UI_OK) {
                report(st, lineno, "out of memory", copy);
                continue;
            }
            for (size_t i = 0; i <= n; i++) got[i] = got[i] != UI_LINE_BREAK_NONE;
        } else {
            ui_grapheme_breaks(cps, n, got);
        }
        if (memcmp(got, expected, n + 1)) {
            char what[64 + 2 * MAX_CPS] = "got";
            size_t len = strlen(what);
            for (size_t i = 0; i <= n && len + 3 < sizeof what; i++) {
                what[len++] = ' ';
                what[len++] = got[i] ? '/' : 'x';
            }
            what[len] = 0;
            report(st, lineno, what, copy);
        }
    }
    ui_line_breaker_free(&lb);
}

static void run_bidi(FILE *f, stats *st) {
    static char buf[1 << 16], copy[1 << 16];
    uint32_t cps[MAX_CPS];
    int expected_levels[MAX_CPS];
    uint32_t expected_order[MAX_CPS], kept[MAX_CPS], visual[MAX_CPS];
    uint8_t line_levels[MAX_CPS], kept_levels[MAX_CPS];
    ui_bidi b = {0};
    unsigned lineno = 0;
    while (fgets(buf, sizeof buf, f)) {
        lineno++;
        if (buf[0] == '#' || buf[0] == '\n' || buf[0] == '\r') continue;
        memcpy(copy, buf, sizeof buf);
        char *fields[5];
        char *p = buf;
        for (int k = 0; k < 5; k++) {
            fields[k] = p;
            char *semi = strchr(p, ';');
            if (semi) {
                *semi = 0;
                p = semi + 1;
            } else {
                p += strlen(p);
            }
        }
        size_t n = 0;
        for (char *tok = strtok(fields[0], " "); tok && n < MAX_CPS; tok = strtok(NULL, " "))
            cps[n++] = (uint32_t)strtoul(tok, NULL, 16);
        int dir_field = atoi(fields[1]);
        int para = atoi(fields[2]);
        size_t nl = 0;
        for (char *tok = strtok(fields[3], " \r\n"); tok && nl < MAX_CPS; tok = strtok(NULL, " \r\n"))
            expected_levels[nl++] = tok[0] == 'x' ? -1 : atoi(tok);
        size_t no = 0;
        for (char *tok = strtok(fields[4], " \r\n"); tok && no < MAX_CPS; tok = strtok(NULL, " \r\n"))
            expected_order[no++] = (uint32_t)atoi(tok);
        st->cases++;
        if (nl != n) {
            report(st, lineno, "unparsable", copy);
            continue;
        }
        ui_bidi_dir dir = dir_field == 0 ? UI_BIDI_LTR : dir_field == 1 ? UI_BIDI_RTL : UI_BIDI_AUTO;
        if (ui_bidi_resolve(&b, cps, n, dir) != UI_OK) {
            report(st, lineno, "out of memory", copy);
            continue;
        }
        if (n && b.para_level[0] != para) {
            report(st, lineno, "paragraph level", copy);
            continue;
        }
        ui_bidi_line_levels(&b, 0, (uint32_t)n, line_levels);
        bool ok = true;
        uint32_t nk = 0;
        for (size_t i = 0; i < n; i++) {
            bool removed = ui_bidi_removed(b.classes[i]);
            if (removed != (expected_levels[i] < 0) ||
                (!removed && line_levels[i] != expected_levels[i])) {
                ok = false;
                break;
            }
            if (!removed) {
                kept[nk] = (uint32_t)i;
                kept_levels[nk++] = line_levels[i];
            }
        }
        if (!ok) {
            report(st, lineno, "levels", copy);
            continue;
        }
        ui_bidi_reorder(kept_levels, nk, visual);
        if (nk != no) ok = false;
        for (uint32_t k = 0; ok && k < nk; k++) ok = kept[visual[k]] == expected_order[k];
        if (!ok) report(st, lineno, "order", copy);
    }
    ui_bidi_free(&b);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s bidi|linebreak|grapheme FILE\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[2], "rb");
    if (!f) {
        perror(argv[2]);
        return 2;
    }
    stats st = {0};
    if (!strcmp(argv[1], "bidi")) run_bidi(f, &st);
    else if (!strcmp(argv[1], "linebreak")) run_segmentation(f, true, &st);
    else if (!strcmp(argv[1], "grapheme")) run_segmentation(f, false, &st);
    else return 2;
    fclose(f);
    fprintf(stderr, "UCD %s %s: %u cases, %u failures\n", UI_UCD_VERSION, argv[1], st.cases,
            st.failures);
    return st.failures || !st.cases;
}
