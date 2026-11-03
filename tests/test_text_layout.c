// Paragraph layout: line breaking (opportunities, mandatory, emergency), hanging
// whitespace, bidi line reordering and alignment, max lines with an ellipsis, whole-pixel
// baselines, reshaping at unsafe breaks, the two-level cache and the measure memo.
#include <math.h>
#include <string.h>

#include "test.h"
#include "text/paragraph.h"

#define FONT(name) UI_TEST_FONTS "/" name

static ui_logger quiet = {UI_LOG_WARN};
static ui_font_registry *fonts;
static ui_text_context ctx;

static const char LOREM[] =
    "The quick brown fox jumps over the lazy dog. Pack my box with five dozen liquor jugs, "
    "then sphinx of black quartz judge my vow.";

static void setup(void) {
    ui_font_registry_desc desc = {.log = &quiet};
    ui_font_registry_create(&desc, &fonts);
    const char *files[] = {FONT("NotoSans-VF.ttf"), FONT("NotoSansHebrew-VF.ttf"),
                           FONT("NotoSansArabic-VF.ttf"), FONT("NotoSansSC-Subset.ttf")};
    for (size_t i = 0; i < UI_ARRAY_LEN(files); i++) ui_font_registry_add_file(fonts, files[i], 0, NULL);
    ui_text_context_init(&ctx, fonts);
}

static void set(ui_paragraph *p, const char *text, uint32_t max_lines, ui_bidi_dir dir) {
    static const uint32_t offsets[] = {0, 9};
    ui_paragraph_desc d = {
        .text = (const uint8_t *)text, .len = (uint32_t)strlen(text),
        .base = {.family = 0, .weight = 400, .size = 16, .color = {0, 0, 0, 1}},
        .families = "Noto Sans", .family_offsets = offsets, .family_count = 1,
        .dir = dir, .max_lines = max_lines,
        .ellipsis = (const uint8_t *)"\xE2\x80\xA6", .ellipsis_len = 3,
    };
    CHECK(ui_paragraph_set(p, &d) == UI_OK);
}

static const ui_paragraph_layout *lay(ui_paragraph *p, float scale, float width) {
    ui_result res;
    const ui_paragraph_layout *l = ui_paragraph_layout_get(p, &ctx, scale, width, &res);
    CHECK(res == UI_OK && l);
    return l;
}

static void test_wrapping(void) {
    ui_paragraph p = {0};
    set(&p, LOREM, 0, UI_BIDI_AUTO);
    const ui_paragraph_layout *one = lay(&p, 1, INFINITY);
    CHECK_EQ_INT(one->line_count, 1);
    float full = one->width;
    CHECK(full > 600);

    const ui_paragraph_layout *l = lay(&p, 1, 200);
    CHECK(l->line_count >= 4);
    CHECK_EQ_INT(l->lines[0].start, 0);
    for (uint32_t i = 0; i < l->line_count; i++) {
        const ui_line *ln = &l->lines[i];
        CHECK(ln->width <= 200 + 0.02f);
        CHECK(ln->width > 100); // greedy filling: no line is mostly empty
        if (i) CHECK_EQ_INT(ln->start, l->lines[i - 1].end);
        if (i + 1 < l->line_count) CHECK(LOREM[ln->end - 1] == ' '); // breaks after spaces
        // Whole-pixel baselines and line boxes.
        CHECK(ln->baseline == roundf(ln->baseline));
        CHECK(ln->height == roundf(ln->height));
        CHECK(ln->baseline > ln->top && ln->baseline < ln->top + ln->height);
        if (i) CHECK_EQ_INT(ln->top, l->lines[i - 1].top + l->lines[i - 1].height);
    }
    CHECK_EQ_INT(l->lines[l->line_count - 1].end, strlen(LOREM));
    CHECK(l->width <= 200 + 0.02f);
    CHECK_NEAR(l->height, (double)l->line_count * l->lines[0].height, 0.01);

    // Glyphs are positioned left to right within each LTR line.
    for (uint32_t i = 0; i < l->line_count; i++) {
        const ui_line *ln = &l->lines[i];
        float x = -1;
        for (uint32_t r = 0; r < ln->run_count; r++) {
            const ui_layout_run *run = &l->runs[ln->run_start + r];
            for (uint32_t g = 0; g < run->glyph_count; g++) {
                const ui_layout_glyph *gl = &l->glyphs[run->glyph_start + g];
                CHECK(gl->x >= x);
                CHECK_EQ_INT(gl->y, ln->baseline);
                x = gl->x;
            }
        }
    }
    ui_paragraph_free(&p);
}

static void test_breaks(void) {
    ui_paragraph p = {0};
    // Mandatory breaks, including a trailing one that leaves an empty last line.
    set(&p, "one\ntwo\n", 0, UI_BIDI_AUTO);
    const ui_paragraph_layout *l = lay(&p, 1, INFINITY);
    CHECK_EQ_INT(l->line_count, 3);
    CHECK_EQ_INT(l->lines[0].end, 4);
    CHECK_EQ_INT(l->lines[1].end, 8);
    CHECK_EQ_INT(l->lines[2].start, 8);
    CHECK_EQ_INT(l->lines[2].run_count, 0);
    CHECK(l->lines[2].height > 0);

    // Emergency breaks: a word wider than the line breaks between graphemes.
    set(&p, "Supercalifragilisticexpialidocious", 0, UI_BIDI_AUTO);
    l = lay(&p, 1, 60);
    CHECK(l->line_count >= 4);
    for (uint32_t i = 0; i < l->line_count; i++) {
        CHECK(l->lines[i].end > l->lines[i].start);
        CHECK(l->lines[i].width <= 60 + 0.02f);
    }
    // Narrower than one grapheme: one grapheme per line, never an empty line.
    l = lay(&p, 1, 1);
    CHECK_EQ_INT(l->line_count, 34);

    // Trailing whitespace hangs: it doesn't count toward the width or force a break.
    set(&p, "aaa     aaa", 0, UI_BIDI_AUTO);
    float aaa_w;
    {
        ui_paragraph q = {0};
        set(&q, "aaa", 0, UI_BIDI_AUTO);
        aaa_w = lay(&q, 1, INFINITY)->width;
        ui_paragraph_free(&q);
    }
    l = lay(&p, 1, aaa_w + 2);
    CHECK_EQ_INT(l->line_count, 2);
    CHECK_EQ_INT(l->lines[0].end, 8);
    CHECK_NEAR(l->lines[0].width, aaa_w, 0.01);

    // Empty text still has one line.
    set(&p, "", 0, UI_BIDI_AUTO);
    l = lay(&p, 1, 100);
    CHECK_EQ_INT(l->line_count, 1);
    CHECK(l->height > 10);
    ui_paragraph_free(&p);
}

static void test_bidi_lines(void) {
    ui_paragraph p = {0};
    // LTR paragraph with a Hebrew word: three visual runs, Hebrew glyphs reversed.
    set(&p, "abc \xD7\x90\xD7\x91\xD7\x92 def", 0, UI_BIDI_AUTO);
    const ui_paragraph_layout *l = lay(&p, 1, INFINITY);
    CHECK_EQ_INT(l->line_count, 1);
    const ui_line *ln = &l->lines[0];
    CHECK_EQ_INT(ln->x, 0);
    CHECK(ln->run_count >= 3);
    const ui_layout_run *heb = NULL;
    for (uint32_t r = 0; r < ln->run_count; r++) {
        const ui_layout_run *run = &l->runs[ln->run_start + r];
        if (run->level == 1) heb = run;
        if (r) CHECK(run->x >= l->runs[ln->run_start + r - 1].x);
    }
    CHECK(heb != NULL);
    if (heb) {
        CHECK_EQ_INT(heb->start, 4);
        const ui_layout_glyph *g = &l->glyphs[heb->glyph_start];
        CHECK(g[0].cluster > g[heb->glyph_count - 1].cluster); // alef is rightmost
        CHECK_EQ_INT(g[heb->glyph_count - 1].cluster, 4);
    }

    // RTL paragraph: lines align right; trailing whitespace hangs off the left edge.
    set(&p, "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D \xD7\xA2\xD7\x95\xD7\x9C\xD7\x9D "
            "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D", 0, UI_BIDI_AUTO);
    l = lay(&p, 1, 300);
    CHECK_EQ_INT(l->para_level, 1);
    CHECK_EQ_INT(l->line_count, 1);
    CHECK_NEAR(l->lines[0].x + l->lines[0].width, 300, 0.01);
    float narrow_w = l->lines[0].width * 0.7f;
    l = lay(&p, 1, narrow_w);
    CHECK_EQ_INT(l->line_count, 2);
    for (uint32_t i = 0; i < l->line_count; i++) {
        CHECK_NEAR(l->lines[i].x + l->lines[i].width, narrow_w, 0.01);
        // The first visual glyph of line 0 is the trailing space, left of the content.
        if (i == 0) CHECK(l->glyphs[l->runs[l->lines[0].run_start].glyph_start].x < l->lines[0].x);
    }
    ui_paragraph_free(&p);
}

static void test_ellipsis(void) {
    ui_paragraph p = {0};
    set(&p, LOREM, 2, UI_BIDI_AUTO);
    const ui_paragraph_layout *l = lay(&p, 1, 150);
    CHECK_EQ_INT(l->line_count, 2);
    CHECK(l->truncated);
    const ui_line *last = &l->lines[1];
    CHECK(last->ellipsized);
    CHECK(!l->lines[0].ellipsized);
    CHECK(last->width <= 150 + 0.02f);
    const ui_layout_run *er = &l->runs[last->run_start + last->run_count - 1];
    CHECK(er->ellipsis); // logical end of an LTR line: rightmost
    CHECK_EQ_INT(er->start, last->end);
    CHECK(LOREM[last->end - 1] != ' '); // no trailing space before the ellipsis

    // Fits entirely: no ellipsis.
    l = lay(&p, 1, INFINITY);
    CHECK_EQ_INT(l->line_count, 1);
    CHECK(!l->truncated && !l->lines[0].ellipsized);

    // RTL: the ellipsis goes on the left.
    set(&p, "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D \xD7\xA2\xD7\x95\xD7\x9C\xD7\x9D "
            "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D \xD7\xA2\xD7\x95\xD7\x9C\xD7\x9D", 1, UI_BIDI_AUTO);
    l = lay(&p, 1, 80);
    CHECK_EQ_INT(l->line_count, 1);
    CHECK(l->lines[0].ellipsized);
    CHECK(l->runs[l->lines[0].run_start].ellipsis);
    ui_paragraph_free(&p);
}

static void test_reshape_at_unsafe_break(void) {
    // An emergency break inside an Arabic word falls between joined letters, which HarfBuzz
    // flags unsafe to break, so the pieces are reshaped. They are reshaped with the rest of
    // the paragraph as context, so the letters keep the joining forms of the whole word
    // (CSS Text 3 §7.3, shaping across intra-word breaks): per cluster, the broken lines
    // show the same glyphs as the unbroken word.
    ui_paragraph p = {0}, whole = {0};
    const char *word = "\xD8\xA8\xD8\xA8\xD8\xA8\xD8\xA8\xD8\xA8\xD8\xA8"; // six beh
    set(&whole, word, 0, UI_BIDI_AUTO);
    const ui_paragraph_layout *full = lay(&whole, 1, INFINITY);
    set(&p, word, 0, UI_BIDI_AUTO);
    const ui_paragraph_layout *l = lay(&p, 1, full->width * 0.6f);
    CHECK_EQ_INT(l->line_count, 2);
    uint32_t total = 0;
    for (uint32_t i = 0; i < l->glyph_count; i++) {
        bool found = false;
        for (uint32_t j = 0; j < full->glyph_count && !found; j++)
            found = full->glyphs[j].cluster == l->glyphs[i].cluster &&
                    full->glyphs[j].glyph == l->glyphs[i].glyph;
        CHECK(found);
        total++;
    }
    CHECK_EQ_INT(total, full->glyph_count);
    ui_paragraph_free(&whole);
    ui_paragraph_free(&p);
}

static void set_para(ui_paragraph *p, const char *text, ui_align align, float line_height) {
    static const uint32_t offsets[] = {0, 9};
    ui_paragraph_desc d = {
        .text = (const uint8_t *)text, .len = (uint32_t)strlen(text),
        .base = {.family = 0, .weight = 400, .size = 16, .color = {0, 0, 0, 1}},
        .families = "Noto Sans", .family_offsets = offsets, .family_count = 1,
        .align = align, .line_height = line_height,
    };
    CHECK(ui_paragraph_set(p, &d) == UI_OK);
}

static void test_alignment(void) {
    ui_paragraph p = {0};
    const float W = 300;
    set_para(&p, "short line", UI_ALIGN_CENTER, 0);
    const ui_paragraph_layout *l = lay(&p, 1, W);
    CHECK_NEAR(l->lines[0].x, (W - l->lines[0].width) / 2, 0.01);
    CHECK_NEAR(l->glyphs[0].x, l->lines[0].x, 0.01);
    set_para(&p, "short line", UI_ALIGN_RIGHT, 0);
    l = lay(&p, 1, W);
    CHECK_NEAR(l->lines[0].x + l->lines[0].width, W, 0.01);
    set_para(&p, "short line", UI_ALIGN_END, 0); // LTR: end is right
    CHECK_NEAR(lay(&p, 1, W)->lines[0].x + lay(&p, 1, W)->lines[0].width, W, 0.01);
    // RTL: start is right, end and left are left.
    set_para(&p, "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D", UI_ALIGN_START, 0);
    l = lay(&p, 1, W);
    CHECK_NEAR(l->lines[0].x + l->lines[0].width, W, 0.01);
    set_para(&p, "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D", UI_ALIGN_END, 0);
    CHECK_NEAR(lay(&p, 1, W)->lines[0].x, 0, 0.01);

    // Justify: every line but the last fills the width; spaces take the extra room; the
    // trailing (hanging) space of a line does not.
    set_para(&p, LOREM, UI_ALIGN_JUSTIFY, 0);
    l = lay(&p, 1, W);
    CHECK(l->line_count >= 3);
    for (uint32_t i = 0; i < l->line_count; i++) {
        const ui_line *ln = &l->lines[i];
        bool last = i + 1 == l->line_count;
        CHECK_NEAR(ln->x, 0, 0.01);
        if (!last) CHECK_NEAR(ln->width, W, 0.01);
        else CHECK(ln->width < W - 1);
        // The last visible glyph ends at the right edge.
        if (!last) {
            float right = 0;
            for (uint32_t r = 0; r < ln->run_count; r++) {
                const ui_layout_run *run = &l->runs[ln->run_start + r];
                for (uint32_t g = 0; g < run->glyph_count; g++) {
                    const ui_layout_glyph *gl = &l->glyphs[run->glyph_start + g];
                    if (gl->cluster < ln->end - 1) right = fmaxf(right, gl->x + gl->advance);
                }
            }
            CHECK_NEAR(right, W, 0.05);
        }
    }
    // A line without spaces (an emergency break) is start-aligned, not stretched.
    set_para(&p, "Supercalifragilisticexpialidocious", UI_ALIGN_JUSTIFY, 0);
    l = lay(&p, 1, 80);
    CHECK(l->lines[0].width < 80);
    // A hard break ends a paragraph: the line before it is not justified.
    set_para(&p, "a b\nc d e f", UI_ALIGN_JUSTIFY, 0);
    l = lay(&p, 1, W);
    CHECK(l->lines[0].width < W - 1);
    ui_paragraph_free(&p);
}

static void test_line_height(void) {
    ui_paragraph p = {0};
    // Fixed line boxes: every line is round(1.5 * 16) px, even one with a Hebrew fallback
    // word whose font has taller metrics, and baselines are evenly spaced.
    set_para(&p, "Latin words here and then \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D more words to wrap", UI_ALIGN_START, 1.5f);
    const ui_paragraph_layout *l = lay(&p, 1, 120);
    CHECK(l->line_count >= 3);
    for (uint32_t i = 0; i < l->line_count; i++) {
        CHECK_EQ_INT(l->lines[i].height, 24);
        if (i) CHECK_EQ_INT(l->lines[i].baseline - l->lines[i - 1].baseline, 24);
        CHECK(l->lines[i].baseline == roundf(l->lines[i].baseline));
    }
    CHECK_EQ_INT(l->height, 24 * l->line_count);
    // At 1.25x the box scales with the font.
    l = lay(&p, 1.25f, 150);
    CHECK_EQ_INT(l->lines[0].height, 30);
    // Empty text gets the same box.
    set_para(&p, "", UI_ALIGN_START, 2.f);
    CHECK_EQ_INT(lay(&p, 1, 100)->lines[0].height, 32);
    ui_paragraph_free(&p);
}

static void test_controls(void) {
    // Line breaks and other controls draw nothing and take no width (fonts map them to
    // .notdef, which would draw a box); a tab advances to the next stop, 8 spaces apart.
    ui_paragraph p = {0}, q = {0};
    set(&q, "ab", 0, UI_BIDI_AUTO);
    float ab = lay(&q, 1, INFINITY)->width;
    set(&p, "ab\n", 0, UI_BIDI_AUTO);
    const ui_paragraph_layout *l = lay(&p, 1, INFINITY);
    CHECK_NEAR(l->lines[0].width, ab, 0.01);
    for (uint32_t i = 0; i < l->glyph_count; i++)
        if (l->glyphs[i].cluster == 2) CHECK(l->glyphs[i].glyph == UI_GLYPH_INVISIBLE && l->glyphs[i].advance == 0);
    set(&p, "a\x01" "b\r\n", 0, UI_BIDI_AUTO);
    l = lay(&p, 1, INFINITY);
    CHECK_NEAR(l->lines[0].width, ab, 0.01);

    set(&q, " ", 0, UI_BIDI_AUTO);
    float space = lay(&q, 1, INFINITY)->glyphs[0].advance;
    set(&p, "a\tb\t\tc", 0, UI_BIDI_AUTO);
    l = lay(&p, 1, INFINITY);
    float stop = space * 8;
    for (uint32_t i = 0; i < l->glyph_count; i++) {
        const ui_layout_glyph *g = &l->glyphs[i];
        if (g->cluster == 2) CHECK_NEAR(g->x, stop, 0.01);     // 'b' at the first stop
        if (g->cluster == 5) CHECK_NEAR(g->x, 3 * stop, 0.01); // 'c' two stops later
        if (g->cluster == 1) CHECK(g->glyph == UI_GLYPH_INVISIBLE);
    }
    // Line breaking measures tabs the same way.
    l = lay(&p, 1, stop * 2);
    CHECK_EQ_INT(l->line_count, 2);
    ui_paragraph_free(&q);
    ui_paragraph_free(&p);
}

static void test_cache(void) {
    ui_paragraph p = {0};
    set(&p, LOREM, 0, UI_BIDI_AUTO);
    lay(&p, 1, 200);
    CHECK_EQ_INT(p.shape_count, 1);
    CHECK_EQ_INT(p.layout_count, 1);
    lay(&p, 1, 200);
    CHECK_EQ_INT(p.layout_count, 1);
    lay(&p, 1, 250); // a width change re-breaks lines without reshaping
    CHECK_EQ_INT(p.shape_count, 1);
    CHECK_EQ_INT(p.layout_count, 2);
    const ui_paragraph_layout *l = lay(&p, 1.25f, 250); // a scale change reshapes
    CHECK_EQ_INT(p.shape_count, 2);
    CHECK_EQ_INT(p.layout_count, 3);
    for (uint32_t i = 0; i < l->line_count; i++) CHECK(l->lines[i].baseline == roundf(l->lines[i].baseline));
    set(&p, "other", 0, UI_BIDI_AUTO); // new text invalidates both
    lay(&p, 1.25f, 250);
    CHECK_EQ_INT(p.shape_count, 3);
    ui_paragraph_free(&p);
}

// New text forgets the measurements. Regression: entries then filled from where the old
// ones stopped while lookups scanned from the first, so a width measured before the change
// answered with the old text's size.
static void test_measure_memo(void) {
    ui_paragraph p = {0}, fresh = {0};
    ui_text_measure m, want;
    set(&p, LOREM, 0, UI_BIDI_AUTO);
    CHECK(ui_paragraph_measure(&p, &ctx, 1, 100, &m) == UI_OK);
    CHECK(ui_paragraph_measure(&p, &ctx, 1, 200, &m) == UI_OK);
    uint64_t layouts = p.layout_count;
    CHECK(ui_paragraph_measure(&p, &ctx, 1, 100, &m) == UI_OK); // remembered
    CHECK_EQ_INT(p.layout_count, layouts);
    set(&p, "short", 0, UI_BIDI_AUTO);
    CHECK(ui_paragraph_measure(&p, &ctx, 1, 300, &m) == UI_OK);
    CHECK(ui_paragraph_measure(&p, &ctx, 1, 100, &m) == UI_OK);
    set(&fresh, "short", 0, UI_BIDI_AUTO);
    CHECK(ui_paragraph_measure(&fresh, &ctx, 1, 100, &want) == UI_OK);
    CHECK(m.width == want.width && m.height == want.height && m.baseline == want.baseline);
    ui_paragraph_free(&p);
    ui_paragraph_free(&fresh);
}

int main(void) {
    setup();
    RUN(test_wrapping);
    RUN(test_breaks);
    RUN(test_bidi_lines);
    RUN(test_ellipsis);
    RUN(test_reshape_at_unsafe_break);
    RUN(test_alignment);
    RUN(test_line_height);
    RUN(test_controls);
    RUN(test_cache);
    RUN(test_measure_memo);
    ui_text_context_free(&ctx);
    ui_font_registry_destroy(fonts);
    return test_failures != 0;
}
