// Unit tests for the Unicode layer beyond the conformance files: UTF-8 replacement policy,
// the table version, script runs, and bidi details the conformance files don't isolate.
#include <stdlib.h>
#include <string.h>

#include "runtime/common.h"
#include "test.h"
#include "unicode/bidi.h"
#include "unicode/script.h"
#include "unicode/segment.h"
#include "unicode/ucd.h"
#include "unicode/utf8.h"

static size_t decode(const char *s, uint32_t *cps) {
    size_t n = 0, i = 0, len = strlen(s);
    while (i < len) cps[n++] = ui_utf8_next((const uint8_t *)s, &i);
    return n;
}

static void test_version(void) {
    CHECK(!strcmp(UI_UCD_VERSION, "18.0.0"));
    CHECK_EQ_INT(ui_ucd('A')->lb, UI_LB_AL);
    CHECK_EQ_INT(ui_ucd(0x05D0)->bidi, UI_BC_R);
    CHECK_EQ_INT(ui_ucd(0x0627)->bidi, UI_BC_AL);
    CHECK_EQ_INT(ui_ucd(0x4E00)->script, UI_SCRIPT_HANI);
    CHECK_EQ_INT(ui_script_tag(UI_SCRIPT_LATN), 0x4C61746E); // 'Latn'
    CHECK_EQ_INT(ui_ucd(0x00AD)->lb, UI_LB_HH);  // changed in Unicode 18 (was BA)
    CHECK_EQ_INT(ui_ucd(0x2013)->lb, UI_LB_BA);  // changed in Unicode 18 (was HH)
    CHECK_EQ_INT(ui_ucd_incb(ui_ucd(0x094D)), UI_INCB_LINKER);
    CHECK(ui_ucd(0x1F600)->flags & UI_UCD_EXTPICT);
    CHECK(ui_ucd(0x200B)->flags & UI_UCD_DEFAULT_IGNORABLE);
    CHECK(ui_ucd(0x4E00)->flags & UI_UCD_EAST_ASIAN);
    CHECK(!(ui_ucd('A')->flags & UI_UCD_EAST_ASIAN));
    // Above U+10FFFF behaves as U+FFFD.
    CHECK(ui_ucd(0x110000) == ui_ucd(0xFFFD));
    uint32_t pair;
    bool open;
    CHECK(ui_ucd_paired_bracket('(', &pair, &open) && pair == ')' && open);
    CHECK(ui_ucd_paired_bracket(0x232A, &pair, &open) && pair == 0x2329 && !open);
    CHECK(!ui_ucd_paired_bracket('a', &pair, &open));
}

static void check_sanitize(const char *in, size_t n, const char *expected, uint32_t replaced) {
    uint8_t *out;
    size_t out_n;
    uint32_t count;
    CHECK(ui_utf8_sanitize((const uint8_t *)in, n, &out, &out_n, &count) == UI_OK);
    CHECK_EQ_INT(count, replaced);
    CHECK_EQ_INT(out_n, strlen(expected));
    CHECK(!memcmp(out, expected, out_n));
    CHECK(ui_utf8_valid(out, out_n));
    free(out);
}

#define R "\xEF\xBF\xBD" // U+FFFD

static void test_utf8(void) {
    // Unicode §3.9, Table 3-8: maximal subparts each become one U+FFFD.
    const char t38[] = "\x61\xF1\x80\x80\xE1\x80\xC2\x62\x80\x63\x80\xBF\x64";
    check_sanitize(t38, sizeof t38 - 1, "a" R R R "b" R "c" R R "d", 6);
    check_sanitize("\xED\xA0\x80", 3, R R R, 3);          // surrogate: ED's 2nd byte is 80..9F
    check_sanitize("\xC0\x80", 2, R R, 2);                // overlong lead
    check_sanitize("\xE0\x80\x80", 3, R R R, 3);          // overlong 3-byte
    check_sanitize("\xF4\x90\x80\x80", 4, R R R R, 4);    // above U+10FFFF
    check_sanitize("\xF0\x9F\x98", 3, R, 1);              // truncated at end of text
    check_sanitize("ok \xF0\x9F\x98\x80", 7, "ok \xF0\x9F\x98\x80", 0);
    check_sanitize("", 0, "", 0);
    CHECK(!ui_utf8_valid((const uint8_t *)"\xFF", 1));

    uint8_t buf[4];
    const uint32_t samples[] = {0x24, 0xA2, 0x20AC, 0xD55C, 0x10348, 0x10FFFF};
    for (size_t i = 0; i < UI_ARRAY_LEN(samples); i++) {
        uint32_t len = ui_utf8_encode(samples[i], buf), dl;
        size_t pos = 0;
        CHECK(ui_utf8_decode(buf, len, &dl) == (int32_t)samples[i] && dl == len);
        CHECK(ui_utf8_next(buf, &pos) == samples[i] && pos == len);
    }
}

static void check_runs(const char *text, const uint8_t *expected) {
    uint32_t cps[128];
    uint8_t got[128];
    size_t n = decode(text, cps);
    ui_script_runs(cps, n, got);
    for (size_t i = 0; i < n; i++) {
        if (got[i] != expected[i]) {
            fprintf(stderr, "  \"%s\" index %zu: got %u, want %u\n", text, i, got[i], expected[i]);
            test_failures++;
            return;
        }
    }
}

static void test_scripts(void) {
    enum { L = UI_SCRIPT_LATN, H = UI_SCRIPT_HEBR, D = UI_SCRIPT_DEVA, C = UI_SCRIPT_ZYYY,
           A = UI_SCRIPT_ARAB, J = UI_SCRIPT_HANI, K = UI_SCRIPT_HIRA };
    check_runs("Hi, you", (const uint8_t[]){L, L, L, L, L, L, L});
    // Leading Common joins the first real script; a space after Latin stays Latin.
    check_runs("12 ab \xD7\x90\xD7\x91", (const uint8_t[]){L, L, L, L, L, L, H, H});
    // The closing bracket matches its opener's script even after another script's run.
    check_runs("a(\xD7\x90)b", (const uint8_t[]){L, L, H, L, L});
    check_runs("123", (const uint8_t[]){C, C, C});
    // Devanagari with combining marks and a danda (Common, Script_Extensions has Deva).
    check_runs("\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xB7 \xE0\xA5\xA4",
               (const uint8_t[]){D, D, D, D, D});
    // Arabic comma (Common with Script_Extensions) stays in an Arabic run.
    check_runs("\xD8\xA8\xD8\x8C\xD8\xA8", (const uint8_t[]){A, A, A});
    check_runs("\xE6\x97\xA5\xE3\x81\xAE", (const uint8_t[]){J, K});
}

static void test_bidi_basics(void) {
    ui_bidi b = {0};
    uint32_t cps[64];
    uint8_t levels[64];
    uint32_t visual[64];
    // "abc ABC" with Hebrew: LTR paragraph, Hebrew at level 1, reversed in place.
    size_t n = decode("ab \xD7\x90\xD7\x91\xD7\x92 cd", cps);
    CHECK(ui_bidi_resolve(&b, cps, n, UI_BIDI_AUTO) == UI_OK);
    CHECK_EQ_INT(b.para_level[0], 0);
    ui_bidi_line_levels(&b, 0, (uint32_t)n, levels);
    const uint8_t want[] = {0, 0, 0, 1, 1, 1, 0, 0, 0};
    CHECK(!memcmp(levels, want, n));
    ui_bidi_reorder(levels, (uint32_t)n, visual);
    const uint32_t order[] = {0, 1, 2, 5, 4, 3, 6, 7, 8};
    CHECK(!memcmp(visual, order, sizeof order));

    // Auto direction per paragraph: Hebrew first, then a Latin paragraph after LF.
    n = decode("\xD7\x90 a\nb \xD7\x90", cps);
    CHECK(ui_bidi_resolve(&b, cps, n, UI_BIDI_AUTO) == UI_OK);
    CHECK_EQ_INT(b.para_level[0], 1);
    CHECK_EQ_INT(b.para_level[n - 1], 0);

    // Trailing whitespace takes the paragraph level (L1) in an RTL paragraph.
    n = decode("\xD7\x90 a  ", cps);
    CHECK(ui_bidi_resolve(&b, cps, n, UI_BIDI_RTL) == UI_OK);
    ui_bidi_line_levels(&b, 0, (uint32_t)n, levels);
    CHECK_EQ_INT(levels[n - 1], 1);
    CHECK_EQ_INT(levels[n - 2], 1);
    CHECK_EQ_INT(levels[2], 2);

    // Deep nesting is bounded by max_depth without overflowing anything.
    for (size_t i = 0; i < 64; i++) cps[i] = i % 2 ? 0x202B : 0x202A; // RLE / LRE
    CHECK(ui_bidi_resolve(&b, cps, 64, UI_BIDI_LTR) == UI_OK);

    // Many bracket pairs in one sequence (more than the BD16 stack depth). In "ב(ג) x" the
    // pair encloses R with R before it, so N0 makes ")" R; without N0 it would be L (N2).
    const uint32_t group[] = {0x05D1, '(', 0x05D2, ')', ' ', 'x', ' '};
    uint32_t *many = malloc(7 * 600 * sizeof *many);
    for (size_t i = 0; i < 7 * 600; i++) many[i] = group[i % 7];
    CHECK(ui_bidi_resolve(&b, many, 7 * 600, UI_BIDI_LTR) == UI_OK);
    CHECK_EQ_INT(b.levels[3], 1);
    CHECK_EQ_INT(b.levels[7 * 599 + 3], 1); // the last pair resolved like the first
    free(many);
    ui_bidi_free(&b);
}

static void test_segmentation_basics(void) {
    uint32_t cps[64];
    uint8_t out[65];
    // Emoji ZWJ sequence and a flag are single grapheme clusters.
    size_t n = decode("\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5", cps);
    ui_grapheme_breaks(cps, n, out);
    const uint8_t want_g[] = {1, 0, 0, 1, 0, 1};
    CHECK(!memcmp(out, want_g, n + 1));

    ui_line_breaker lb = {0};
    n = decode("a b\nc", cps);
    CHECK(ui_line_breaks(&lb, cps, n, out) == UI_OK);
    const uint8_t want_l[] = {UI_LINE_BREAK_NONE, UI_LINE_BREAK_NONE, UI_LINE_BREAK_ALLOWED,
                              UI_LINE_BREAK_NONE, UI_LINE_BREAK_MANDATORY, UI_LINE_BREAK_MANDATORY};
    CHECK(!memcmp(out, want_l, n + 1));
    ui_line_breaker_free(&lb);
}

int main(void) {
    RUN(test_version);
    RUN(test_utf8);
    RUN(test_scripts);
    RUN(test_bidi_basics);
    RUN(test_segmentation_basics);
    return test_failures != 0;
}
