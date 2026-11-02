// Font loading, family matching, fallback, Fontconfig discovery in an isolated
// configuration, itemization and shaping. Uses only the fonts in tests/fonts.
#include <string.h>

#include "test.h"
#include "text/fontconfig/fc.h"
#include "text/shape.h"

#define FONT(name) UI_TEST_FONTS "/" name

static ui_logger quiet = {UI_LOG_WARN};
static const char *const all_fonts[] = {
    FONT("NotoSans-VF.ttf"),           FONT("NotoSansHebrew-VF.ttf"),
    FONT("NotoSansArabic-VF.ttf"),     FONT("NotoSansDevanagari-VF.ttf"),
    FONT("NotoSansSC-Subset.ttf"),
};
enum { LATIN, HEBREW, ARABIC, DEVANAGARI, CJK };

static ui_font_registry *registry_with_all(void) {
    ui_font_registry *r;
    ui_font_registry_desc desc = {.log = &quiet};
    if (ui_font_registry_create(&desc, &r) != UI_OK) return NULL;
    for (uint32_t i = 0; i < UI_ARRAY_LEN(all_fonts); i++) {
        uint32_t id;
        CHECK(ui_font_registry_add_file(r, all_fonts[i], 0, &id) == UI_OK);
        CHECK_EQ_INT(id, i);
    }
    return r;
}

static ui_text_input input_for(const char *text, const char *families) {
    static uint32_t offsets[2];
    offsets[0] = 0;
    offsets[1] = (uint32_t)strlen(families);
    return (ui_text_input){
        .text = (const uint8_t *)text,
        .len = (uint32_t)strlen(text),
        .base = {.family = 0, .weight = 400, .style = UI_FONT_NORMAL, .size = 16,
                 .color = {0, 0, 0, 1}},
        .families = families,
        .family_offsets = offsets,
        .family_count = 1,
        .dir = UI_BIDI_AUTO,
    };
}

static void test_faces(void) {
    ui_face *face;
    CHECK(ui_face_load_file(FONT("NotoSans-VF.ttf"), 0, &face) == UI_OK);
    const ui_face_info *info = ui_face_get_info(face);
    CHECK(!strcmp(info->family, "Noto Sans"));
    CHECK(info->has_wght);
    CHECK_NEAR(info->wght_min, 100, 0.01);
    CHECK_NEAR(info->wght_max, 900, 0.01);
    CHECK_EQ_INT(info->weight, 400);
    CHECK(!info->italic);
    CHECK(ui_face_has_codepoint(face, 'A'));
    CHECK(!ui_face_has_codepoint(face, 0x05D0));

    ui_font *font;
    CHECK(ui_font_create(face, 32, NULL, 0, NULL, &font) == UI_OK);
    ui_font_metrics m;
    ui_font_get_metrics(font, &m);
    CHECK(m.ascent > 20 && m.ascent < 40);
    CHECK(m.descent > 4 && m.descent < 20);
    float x0, y0, x1, y1;
    uint32_t a = ui_font_glyph(font, 'A');
    CHECK(a != 0);
    CHECK(ui_font_glyph_bounds(font, a, &x0, &y0, &x1, &y1));
    CHECK(x1 > x0 && y1 > 20 && y0 > -1 && y0 < 1); // 'A' sits on the baseline, cap height ~23
    CHECK(!ui_font_glyph_bounds(font, ui_font_glyph(font, ' '), &x0, &y0, &x1, &y1));
    ui_font_destroy(font);
    ui_face_destroy(face);

    ui_face *bad;
    CHECK(ui_face_load_file(FONT("missing.ttf"), 0, &bad) == UI_ERR_IO);
    CHECK(ui_face_load_file(UI_TEST_FONTS "/SOURCES.md", 0, &bad) == UI_ERR_INVALID_ARGUMENT);
}

static void test_matching(void) {
    ui_font_registry *r = registry_with_all();
    CHECK_EQ_INT(ui_font_registry_match(r, "Noto Sans", 9, 700, UI_FONT_NORMAL), LATIN);
    const char *list = "Missing Font, 'Noto Sans Hebrew', serif";
    CHECK_EQ_INT(ui_font_registry_match(r, list, (uint32_t)strlen(list), 400, UI_FONT_NORMAL),
                 HEBREW);
    // Nothing matches and there is no discovery: the first registered family.
    CHECK_EQ_INT(ui_font_registry_match(r, "Nope", 4, 400, UI_FONT_NORMAL), LATIN);
    CHECK_EQ_INT(ui_font_registry_match(r, "", 0, 400, UI_FONT_NORMAL), LATIN);

    uint32_t hebrew[] = {0x05E9};
    CHECK_EQ_INT(ui_font_registry_resolve(r, LATIN, "", 0, 400, UI_FONT_NORMAL, hebrew, 1), HEBREW);
    uint32_t latin[] = {'a', 0x0301}; // a + combining acute: Noto Sans covers both
    CHECK_EQ_INT(ui_font_registry_resolve(r, LATIN, "", 0, 400, UI_FONT_NORMAL, latin, 2), LATIN);
    uint32_t zwj[] = {'a', 0x200D}; // default-ignorables don't need coverage
    CHECK_EQ_INT(ui_font_registry_resolve(r, LATIN, "", 0, 400, UI_FONT_NORMAL, zwj, 2), LATIN);
    uint32_t none[] = {0x13000}; // Egyptian hieroglyph: nobody has it, so the primary
    CHECK_EQ_INT(ui_font_registry_resolve(r, LATIN, "", 0, 400, UI_FONT_NORMAL, none, 1), LATIN);

    // Instances: same parameters give the same font; weight changes the variation instance.
    ui_font_key k1, k2, k3;
    ui_font *f1 = ui_font_registry_font(r, LATIN, 20, 400, UI_FONT_NORMAL, &k1);
    ui_font *f2 = ui_font_registry_font(r, LATIN, 20, 400, UI_FONT_NORMAL, &k2);
    ui_font *f3 = ui_font_registry_font(r, LATIN, 20, 800, UI_FONT_NORMAL, &k3);
    CHECK(f1 && f1 == f2 && f3 && f3 != f1);
    CHECK(k1.instance == k2.instance && k1.instance != k3.instance);
    CHECK_EQ_INT(k1.size_q, 20 * 64);
    CHECK(ui_font_registry_font_by_key(r, k3) == f3);
    ui_font_registry_destroy(r);
}

static void test_fontconfig_isolated(void) {
    ui_fc *fc;
    const char *dirs[] = {UI_TEST_FONTS};
    CHECK(ui_fc_create(false, dirs, 1, &fc) == UI_OK);
    ui_fc_font f;
    CHECK(ui_fc_match(fc, "Noto Sans Arabic", 400, false, &f));
    CHECK(strstr(f.path, "NotoSansArabic-VF.ttf") != NULL);
    CHECK(!ui_fc_match(fc, "Definitely Not Installed", 400, false, &f));
    uint32_t hebrew[] = {0x05E9, 0x05DC};
    CHECK(ui_fc_fallback(fc, hebrew, 2, 400, false, &f));
    CHECK(strstr(f.path, "NotoSansHebrew-VF.ttf") != NULL);
    uint32_t none[] = {0x13000};
    CHECK(!ui_fc_fallback(fc, none, 1, 400, false, &f));
    ui_fc_destroy(fc);

    // A registry that knows no files finds families and fallbacks through discovery.
    ui_font_registry *r;
    ui_font_registry_desc desc = {.log = &quiet, .font_dirs = dirs, .font_dir_count = 1};
    CHECK(ui_font_registry_create(&desc, &r) == UI_OK);
    uint32_t deva = ui_font_registry_match(r, "Noto Sans Devanagari", 20, 400, UI_FONT_NORMAL);
    CHECK(deva != UI_NO_FACE);
    CHECK(!strcmp(ui_face_get_info(ui_font_registry_face(r, deva))->family, "Noto Sans Devanagari"));
    uint32_t cjk[] = {0x6587};
    uint32_t face = ui_font_registry_resolve(r, deva, "", 0, 400, UI_FONT_NORMAL, cjk, 1);
    CHECK(face != deva && face != UI_NO_FACE);
    CHECK(!strcmp(ui_face_get_info(ui_font_registry_face(r, face))->family, "Noto Sans SC"));
    ui_font_registry_destroy(r);
}

static void test_itemize(void) {
    ui_font_registry *r = registry_with_all();
    ui_text_shaper s;
    CHECK(ui_text_shaper_init(&s) == UI_OK);
    ui_shaped_paragraph p = {0};
    // Latin, Hebrew, Arabic, Devanagari, CJK in an LTR paragraph.
    const char *text = "Hi \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D \xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 "
                       "\xE0\xA4\xA8\xE0\xA4\xAE\xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\xA4\xE0\xA5\x87 "
                       "\xE6\x96\x87\xE5\xAD\x97";
    ui_text_input in = input_for(text, "Noto Sans");
    CHECK(ui_shape_paragraph(&s, r, &in, 1.25f, &p) == UI_OK);
    // Hebrew and Arabic are both RTL (level 1) but split by script and font.
    uint32_t faces[16], scripts[16];
    uint8_t levels[16];
    for (uint32_t i = 0; i < p.run_count && i < 16; i++) {
        faces[i] = p.runs[i].font.face;
        scripts[i] = p.runs[i].script;
        levels[i] = p.runs[i].level;
        CHECK_EQ_INT(p.runs[i].font.size_q, 20 * 64); // 16 px * 1.25
        CHECK(p.runs[i].glyph_count > 0);
    }
    // Every run is contiguous and covers the text in order.
    CHECK_EQ_INT(p.runs[0].start, 0);
    for (uint32_t i = 1; i < p.run_count; i++) CHECK_EQ_INT(p.runs[i].start, p.runs[i - 1].end);
    CHECK_EQ_INT(p.runs[p.run_count - 1].end, in.len);
    // Spaces stay in the font before them (Common text keeps the previous font when it
    // covers it). The space after the Arabic word sits between RTL and LTR text in an LTR
    // paragraph, so it resolves to level 0: its own run, still in the Arabic font. Noto Sans
    // itself covers Devanagari, so the primary font wins there; Hebrew, Arabic and CJK fall
    // back.
    CHECK_EQ_INT(p.run_count, 6);
    const uint32_t want_face[] = {LATIN, HEBREW, ARABIC, ARABIC, LATIN, CJK};
    const uint32_t want_script[] = {0x4C61746E, 0x48656272, 0x41726162, 0x41726162, 0x44657661,
                                    0x48616E69};
    const uint8_t want_level[] = {0, 1, 1, 0, 0, 0};
    for (uint32_t i = 0; i < 6 && i < p.run_count; i++) {
        CHECK_EQ_INT(faces[i], want_face[i]);
        CHECK_EQ_INT(scripts[i], want_script[i]);
        CHECK_EQ_INT(levels[i], want_level[i]);
    }

    // RTL runs come out of HarfBuzz in visual order: clusters never increase (marks share
    // their base's cluster) and the run's last character comes first.
    for (uint32_t i = 0; i < p.run_count; i++) {
        if (!(levels[i] & 1)) continue;
        const ui_glyph_pos *g = p.glyphs + p.runs[i].glyph_start;
        uint32_t n = p.runs[i].glyph_count;
        for (uint32_t k = 1; k < n; k++) CHECK(g[k].cluster <= g[k - 1].cluster);
        CHECK(g[0].cluster > g[n - 1].cluster);
        CHECK_EQ_INT(g[n - 1].cluster, p.runs[i].start);
    }
    ui_shaped_paragraph_free(&p);
    ui_text_shaper_free(&s);
    ui_font_registry_destroy(r);
}

static int32_t total_advance(const ui_shaped_paragraph *p) {
    int32_t sum = 0;
    for (uint32_t i = 0; i < p->glyph_count; i++) sum += p->glyphs[i].advance;
    return sum;
}

static void test_shaping(void) {
    ui_font_registry *r = registry_with_all();
    ui_text_shaper s;
    CHECK(ui_text_shaper_init(&s) == UI_OK);
    ui_shaped_paragraph p = {0};

    // Arabic joining: joined beh uses other base glyphs than the isolated form (Noto draws
    // the dot as a separate mark glyph, so compare glyph sets, not counts).
    ui_text_input in = input_for("\xD8\xA8", "Noto Sans Arabic");
    CHECK(ui_shape_paragraph(&s, r, &in, 1, &p) == UI_OK);
    uint32_t isolated[8];
    uint32_t iso_count = p.glyph_count < 8 ? p.glyph_count : 8;
    for (uint32_t i = 0; i < iso_count; i++) isolated[i] = p.glyphs[i].glyph;
    in = input_for("\xD8\xA8\xD8\xA8\xD8\xA8", "Noto Sans Arabic");
    CHECK(ui_shape_paragraph(&s, r, &in, 1, &p) == UI_OK);
    CHECK_EQ_INT(p.run_count, 1);
    uint32_t new_glyphs = 0;
    for (uint32_t i = 0; i < p.glyph_count; i++) {
        bool known = false;
        for (uint32_t j = 0; j < iso_count; j++) known |= p.glyphs[i].glyph == isolated[j];
        new_glyphs += !known;
    }
    CHECK(new_glyphs >= 2); // initial and medial forms at least

    // Devanagari: the i-matra reorders before the consonant cluster; clusters stay monotone.
    in = input_for("\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xB7\xE0\xA4\xBF", "Noto Sans Devanagari");
    CHECK(ui_shape_paragraph(&s, r, &in, 1, &p) == UI_OK);
    CHECK_EQ_INT(p.run_count, 1);
    CHECK(p.glyph_count >= 1);
    for (uint32_t k = 1; k < p.glyph_count; k++) CHECK(p.glyphs[k].cluster >= p.glyphs[k - 1].cluster);

    // Variable weight: bolder text is wider; scale multiplies advances.
    in = input_for("Hello world", "Noto Sans");
    in.base.weight = 100;
    CHECK(ui_shape_paragraph(&s, r, &in, 1, &p) == UI_OK);
    int32_t thin = total_advance(&p);
    in.base.weight = 900;
    CHECK(ui_shape_paragraph(&s, r, &in, 1, &p) == UI_OK);
    int32_t black = total_advance(&p);
    CHECK(black > thin);
    CHECK(ui_shape_paragraph(&s, r, &in, 2, &p) == UI_OK);
    CHECK_NEAR(total_advance(&p), 2 * black, 2 * 11);

    // Spans: a color-only change does not split the run; a weight change does.
    ui_text_span spans[1] = {{6, 11, in.base}};
    spans[0].style.color[0] = 1;
    in.spans = spans;
    in.span_count = 1;
    CHECK(ui_shape_paragraph(&s, r, &in, 1, &p) == UI_OK);
    CHECK_EQ_INT(p.run_count, 1);
    CHECK_EQ_INT(p.styles[0], 0);
    CHECK_EQ_INT(p.styles[6], 1);
    spans[0].style.weight = 400;
    CHECK(ui_shape_paragraph(&s, r, &in, 1, &p) == UI_OK);
    CHECK_EQ_INT(p.run_count, 2);
    CHECK_EQ_INT(p.runs[1].start, 6);
    CHECK_EQ_INT(ui_shaped_cp_at(&p, 7), 7);

    // Empty text shapes to nothing.
    in = input_for("", "Noto Sans");
    CHECK(ui_shape_paragraph(&s, r, &in, 1, &p) == UI_OK);
    CHECK_EQ_INT(p.run_count, 0);
    CHECK_EQ_INT(p.glyph_count, 0);

    ui_shaped_paragraph_free(&p);
    ui_text_shaper_free(&s);
    ui_font_registry_destroy(r);
}

typedef struct path_count {
    int moves, lines, curves, closes;
} path_count;
static void pc_move(void *u, float x, float y) { (void)x, (void)y, ((path_count *)u)->moves++; }
static void pc_line(void *u, float x, float y) { (void)x, (void)y, ((path_count *)u)->lines++; }
static void pc_quad(void *u, float a, float b, float x, float y) {
    (void)a, (void)b, (void)x, (void)y, ((path_count *)u)->curves++;
}
static void pc_cubic(void *u, float a, float b, float c, float d, float x, float y) {
    (void)a, (void)b, (void)c, (void)d, (void)x, (void)y, ((path_count *)u)->curves++;
}
static void pc_close(void *u) { ((path_count *)u)->closes++; }

static void test_outlines(void) {
    ui_font_registry *r = registry_with_all();
    ui_font *font = ui_font_registry_font(r, LATIN, 40, 400, UI_FONT_NORMAL, NULL);
    path_count c = {0};
    ui_path_sink sink = {pc_move, pc_line, pc_quad, pc_cubic, pc_close, &c};
    CHECK(ui_font_draw_glyph(font, ui_font_glyph(font, 'o'), &sink));
    CHECK_EQ_INT(c.moves, 2); // outer and inner contour
    CHECK(c.curves > 4);
    CHECK_EQ_INT(c.closes, 2);
    c = (path_count){0};
    CHECK(!ui_font_draw_glyph(font, ui_font_glyph(font, ' '), &sink) || c.moves == 0);
    ui_font_registry_destroy(r);
}

static int32_t advance_of(ui_font *font, const char *text) {
    ui_shaper *sh;
    ui_shaper_create(&sh);
    const ui_glyph_pos *g;
    uint32_t n;
    ui_shape_request req = {(const uint8_t *)text, (uint32_t)strlen(text), 0,
                            (uint32_t)strlen(text), 0x4C61746E, false};
    ui_shape(sh, font, &req, &g, &n);
    int32_t sum = 0;
    for (uint32_t i = 0; i < n; i++) sum += g[i].advance;
    ui_shaper_destroy(sh);
    return sum;
}

static void test_synthesis(void) {
    // A static regular face asked for bold gets synthetic emboldening: wider advances, and
    // a separate cache instance.
    ui_font_registry *r;
    ui_font_registry_desc desc = {.log = &quiet};
    CHECK(ui_font_registry_create(&desc, &r) == UI_OK);
    CHECK(ui_font_registry_add_file(r, FONT("NotoSans-Regular-Static.ttf"), 0, NULL) == UI_OK);
    CHECK(ui_font_registry_add_file(r, FONT("NotoSans-VF.ttf"), 0, NULL) == UI_OK);
    CHECK(!ui_face_get_info(ui_font_registry_face(r, 0))->has_wght);
    ui_font_key k400, k700;
    ui_font *regular = ui_font_registry_font(r, 0, 20, 400, UI_FONT_NORMAL, &k400);
    ui_font *bold = ui_font_registry_font(r, 0, 20, 700, UI_FONT_NORMAL, &k700);
    CHECK(k400.instance == 0 && k700.instance != 0);
    CHECK(advance_of(bold, "Hello") > advance_of(regular, "Hello"));
    // 600 is the threshold; 500 is not synthesized.
    ui_font_key k500;
    ui_font_registry_font(r, 0, 20, 500, UI_FONT_NORMAL, &k500);
    CHECK_EQ_INT(k500.instance, 0);

    // A variable face reaches 700 through its axis: no synthesis, same as the plain axis.
    ui_font *vf_bold = ui_font_registry_font(r, 1, 20, 700, UI_FONT_NORMAL, NULL);
    ui_font *axis_bold;
    ui_font_axis wght = {UI_AXIS_WGHT, 700};
    CHECK(ui_font_create(ui_font_registry_face(r, 1), 20, &wght, 1, NULL, &axis_bold) == UI_OK);
    CHECK_EQ_INT(advance_of(vf_bold, "Hello"), advance_of(axis_bold, "Hello"));
    ui_font_destroy(axis_bold);

    // Italic from a face without italic design or axis: a synthetic slant leans the
    // outline right, so the top of an 'l' moves right of the upright one.
    ui_font *upright = ui_font_registry_font(r, 1, 40, 400, UI_FONT_NORMAL, NULL);
    ui_font *slanted = ui_font_registry_font(r, 1, 40, 400, UI_FONT_ITALIC, NULL);
    CHECK(upright != slanted);
    float ux0, uy0, ux1, uy1, sx0, sy0, sx1, sy1;
    uint32_t l = ui_font_glyph(upright, 'l');
    CHECK(ui_font_glyph_bounds(upright, l, &ux0, &uy0, &ux1, &uy1));
    CHECK(ui_font_glyph_bounds(slanted, l, &sx0, &sy0, &sx1, &sy1));
    CHECK(sx1 > ux1 + 3); // ~0.2 * cap height
    ui_font_registry_destroy(r);
}

int main(void) {
    RUN(test_faces);
    RUN(test_matching);
    RUN(test_fontconfig_isolated);
    RUN(test_itemize);
    RUN(test_shaping);
    RUN(test_outlines);
    RUN(test_synthesis);
    return test_failures != 0;
}
