// Hit testing: point -> offset with affinity, offset -> caret, range -> selection rects, for
// Latin (including soft-wrap affinity and hard breaks) and bidi text (carets at direction
// changes under both affinities, split selections).
#include <math.h>
#include <string.h>

#include "test.h"
#include "text/hittest.h"

#define FONT(name) UI_TEST_FONTS "/" name

static ui_logger quiet = {UI_LOG_WARN};
static ui_font_registry *fonts;
static ui_text_context ctx;

static void setup(void) {
    ui_font_registry_desc desc = {.log = &quiet};
    ui_font_registry_create(&desc, &fonts);
    ui_font_registry_add_file(fonts, FONT("NotoSans-VF.ttf"), 0, NULL);
    ui_font_registry_add_file(fonts, FONT("NotoSansHebrew-VF.ttf"), 0, NULL);
    ui_text_context_init(&ctx, fonts);
}

static const ui_paragraph_layout *make(ui_paragraph *p, const char *text, float width) {
    static const uint32_t offsets[] = {0, 9};
    ui_paragraph_desc d = {
        .text = (const uint8_t *)text, .len = (uint32_t)strlen(text),
        .base = {.family = 0, .weight = 400, .size = 20, .color = {0, 0, 0, 1}},
        .families = "Noto Sans", .family_offsets = offsets, .family_count = 1,
    };
    CHECK(ui_paragraph_set(p, &d) == UI_OK);
    ui_result res;
    const ui_paragraph_layout *l = ui_paragraph_layout_get(p, &ctx, 1, width, &res);
    CHECK(l != NULL);
    return l;
}

static float caret_x(ui_paragraph *p, uint32_t offset, ui_affinity aff) {
    ui_text_rect r;
    CHECK(ui_text_caret_rect(p, &ctx, (ui_text_position){offset, aff}, 2, &r) == UI_OK);
    return r.x + 1; // centered on the boundary
}

static float caret_y(ui_paragraph *p, uint32_t offset, ui_affinity aff) {
    ui_text_rect r;
    CHECK(ui_text_caret_rect(p, &ctx, (ui_text_position){offset, aff}, 2, &r) == UI_OK);
    return r.y;
}

static void test_latin(void) {
    ui_paragraph p = {0};
    const char *text = "Hello world";
    const ui_paragraph_layout *l = make(&p, text, INFINITY);
    float mid_y = l->lines[0].top + l->lines[0].height / 2;
    // Every offset round-trips through its caret position.
    float prev = -1;
    for (uint32_t o = 0; o <= strlen(text); o++) {
        float x = caret_x(&p, o, UI_AFFINITY_DOWNSTREAM);
        CHECK(x > prev); // LTR: carets move right
        prev = x;
        ui_text_position pos;
        CHECK(ui_text_hit_point(&p, &ctx, x, mid_y, &pos) == UI_OK);
        CHECK_EQ_INT(pos.offset, o);
    }
    // Leading half of 'e' -> before it (downstream); trailing half -> after it (upstream).
    float e0 = caret_x(&p, 1, UI_AFFINITY_DOWNSTREAM), e1 = caret_x(&p, 2, UI_AFFINITY_DOWNSTREAM);
    ui_text_position pos;
    ui_text_hit_point(&p, &ctx, e0 + (e1 - e0) * 0.25f, mid_y, &pos);
    CHECK(pos.offset == 1 && pos.affinity == UI_AFFINITY_DOWNSTREAM);
    ui_text_hit_point(&p, &ctx, e0 + (e1 - e0) * 0.75f, mid_y, &pos);
    CHECK(pos.offset == 2 && pos.affinity == UI_AFFINITY_UPSTREAM);
    // Clamped outside the text.
    ui_text_hit_point(&p, &ctx, -50, -50, &pos);
    CHECK_EQ_INT(pos.offset, 0);
    ui_text_hit_point(&p, &ctx, 5000, 5000, &pos);
    CHECK_EQ_INT(pos.offset, strlen(text));
    // A selection inside one line is one rectangle from caret to caret.
    ui_text_rect rects[8];
    uint32_t n;
    CHECK(ui_text_selection_rects(&p, &ctx, 1, 4, rects, 8, &n) == UI_OK);
    CHECK_EQ_INT(n, 1);
    CHECK_NEAR(rects[0].x, e0, 0.01);
    CHECK_NEAR(rects[0].x + rects[0].width, caret_x(&p, 4, UI_AFFINITY_DOWNSTREAM), 0.01);
    ui_paragraph_free(&p);
}

static void test_wrap_affinity(void) {
    ui_paragraph p = {0};
    const char *text = "aaaa aaaa";
    ui_paragraph q = {0};
    float w = make(&q, "aaaa", INFINITY)->width;
    ui_paragraph_free(&q);
    const ui_paragraph_layout *l = make(&p, text, w + 4);
    CHECK_EQ_INT(l->line_count, 2);
    CHECK_EQ_INT(l->lines[1].start, 5);
    // Offset 5 is both the end of line 0 (after the space) and the start of line 1.
    CHECK_NEAR(caret_y(&p, 5, UI_AFFINITY_UPSTREAM), l->lines[0].top, 0.01);
    CHECK_NEAR(caret_y(&p, 5, UI_AFFINITY_DOWNSTREAM), l->lines[1].top, 0.01);
    CHECK(caret_x(&p, 5, UI_AFFINITY_UPSTREAM) > w);           // after the hanging space
    CHECK_NEAR(caret_x(&p, 5, UI_AFFINITY_DOWNSTREAM), 0, 0.01); // start of line 1
    // Clicking past the end of line 0 lands upstream at its end.
    ui_text_position pos;
    ui_text_hit_point(&p, &ctx, 1000, l->lines[0].top + 1, &pos);
    CHECK(pos.offset == 5 && pos.affinity == UI_AFFINITY_UPSTREAM);
    // A selection across the wrap: one rect per line.
    ui_text_rect rects[8];
    uint32_t n;
    CHECK(ui_text_selection_rects(&p, &ctx, 2, 7, rects, 8, &n) == UI_OK);
    CHECK_EQ_INT(n, 2);
    CHECK(rects[0].y < rects[1].y);

    // Hard breaks: the caret goes before the newline, never after it on the same line.
    make(&p, "ab\ncd", INFINITY);
    ui_text_hit_point(&p, &ctx, 1000, 1, &pos);
    CHECK_EQ_INT(pos.offset, 2);
    CHECK_NEAR(caret_y(&p, 3, UI_AFFINITY_UPSTREAM), p.layout.lines[1].top, 0.01);
    // A trailing newline makes an empty last line that takes a caret.
    make(&p, "ab\n", INFINITY);
    CHECK_EQ_INT(p.layout.line_count, 2);
    ui_text_hit_point(&p, &ctx, 30, p.layout.lines[1].top + 2, &pos);
    CHECK_EQ_INT(pos.offset, 3);
    CHECK_NEAR(caret_y(&p, 3, UI_AFFINITY_DOWNSTREAM), p.layout.lines[1].top, 0.01);
    ui_paragraph_free(&p);
}

static void test_bidi(void) {
    ui_paragraph p = {0};
    // "abc אבג def": offsets a0 b1 c2 ' '3 א4 ב6 ג8 ' '10 d11 e12 f13, length 14.
    const char *text = "abc \xD7\x90\xD7\x91\xD7\x92 def";
    const ui_paragraph_layout *l = make(&p, text, INFINITY);
    float mid_y = l->lines[0].top + l->lines[0].height / 2;
    float space_end = caret_x(&p, 4, UI_AFFINITY_UPSTREAM);     // after ' ', LTR
    float alef_start = caret_x(&p, 4, UI_AFFINITY_DOWNSTREAM);  // before א, its right edge
    float gimel_end = caret_x(&p, 10, UI_AFFINITY_UPSTREAM);    // after ג, its left edge
    float space2 = caret_x(&p, 10, UI_AFFINITY_DOWNSTREAM);     // before the second ' '
    // The Hebrew run sits between the spaces, reversed: offset 4 has two carets, one at each
    // end of the run, and so has offset 10.
    CHECK_NEAR(space_end, gimel_end, 0.01);
    CHECK_NEAR(alef_start, space2, 0.01);
    CHECK(alef_start - space_end > 20);
    // Inside the run, carets move left as offsets grow.
    float bet = caret_x(&p, 6, UI_AFFINITY_DOWNSTREAM), gimel = caret_x(&p, 8, UI_AFFINITY_DOWNSTREAM);
    CHECK(alef_start > bet && bet > gimel && gimel > gimel_end);
    // Right half of א is its leading half (RTL): before it, downstream; left half: after it.
    ui_text_position pos;
    ui_text_hit_point(&p, &ctx, alef_start - 1, mid_y, &pos);
    CHECK(pos.offset == 4 && pos.affinity == UI_AFFINITY_DOWNSTREAM);
    ui_text_hit_point(&p, &ctx, bet + 1, mid_y, &pos);
    CHECK(pos.offset == 6 && pos.affinity == UI_AFFINITY_UPSTREAM);
    // Hit points map back to their carets.
    for (uint32_t o = 0; o <= 14; o++) {
        if (o == 5 || o == 7 || o == 9) continue; // inside two-byte Hebrew letters
        for (int a = 0; a < 2; a++) {
            float x = caret_x(&p, o, (ui_affinity)a);
            ui_text_hit_point(&p, &ctx, x + (a ? -0.01f : 0.01f), mid_y, &pos);
            CHECK_NEAR(caret_x(&p, pos.offset, pos.affinity), x, 0.02);
        }
    }
    // Selecting " א" (offsets 3..6) gives two pieces: the space, and א at the far end.
    ui_text_rect rects[8];
    uint32_t n;
    CHECK(ui_text_selection_rects(&p, &ctx, 3, 6, rects, 8, &n) == UI_OK);
    CHECK_EQ_INT(n, 2);
    CHECK_NEAR(rects[0].x + rects[0].width, space_end, 0.01);
    CHECK_NEAR(rects[1].x + rects[1].width, alef_start, 0.01);
    // Everything is one rectangle.
    CHECK(ui_text_selection_rects(&p, &ctx, 0, 14, rects, 8, &n) == UI_OK);
    CHECK_EQ_INT(n, 1);
    // An offset inside a character snaps to its start.
    CHECK_NEAR(caret_x(&p, 5, UI_AFFINITY_DOWNSTREAM), alef_start, 0.01);

    // RTL paragraph: offset 0 is at the right edge.
    l = make(&p, "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D abc", 300);
    CHECK_NEAR(caret_x(&p, 0, UI_AFFINITY_DOWNSTREAM), 300, 0.01);
    ui_paragraph_free(&p);
}

static void test_ligature_carets(void) {
    // "ffi" forms a ligature in Noto Sans; the caret can still go between its letters.
    ui_paragraph p = {0};
    const ui_paragraph_layout *l = make(&p, "office", INFINITY);
    CHECK(l->glyph_count < 6); // the ligature formed (one glyph for "ffi")
    float prev = -1;
    for (uint32_t o = 0; o <= 6; o++) {
        float x = caret_x(&p, o, UI_AFFINITY_DOWNSTREAM);
        CHECK(x > prev);
        prev = x;
    }
    ui_paragraph_free(&p);
}

int main(void) {
    setup();
    RUN(test_latin);
    RUN(test_wrap_affinity);
    RUN(test_bidi);
    RUN(test_ligature_carets);
    ui_text_context_free(&ctx);
    ui_font_registry_destroy(fonts);
    return test_failures != 0;
}
