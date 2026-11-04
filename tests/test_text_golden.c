// Golden images of text rendered headless with the bundled fonts, at scales 1.0, 1.25 and
// 1.5, under both schedulers (tests/golden.h has the rules).
#include "golden.h"

#define WIDTH 400u
#define HEIGHT 150u

#define FONT(name) UI_TEST_FONTS "/" name
static const char *const font_files[] = {
    FONT("NotoSans-VF.ttf"),         FONT("NotoSansHebrew-VF.ttf"),
    FONT("NotoSansArabic-VF.ttf"),   FONT("NotoSansDevanagari-VF.ttf"),
    FONT("NotoSansSC-Subset.ttf"),
};

typedef struct golden_case {
    const char *name;
    const char *text;
    float width;        // node width, logical px
    uint16_t max_lines;
    const ui_text_span_desc *spans;
    uint32_t span_count;
    ui_text_align align;
    float line_height;
} golden_case;

#define INK {0.08f, 0.08f, 0.1f, 1}
// "Regular italic bold black, bold italic": italic is synthesized (Noto Sans has no italic
// axis), weights come from the wght axis.
static const ui_text_span_desc style_spans[] = {
    {.start = 8, .end = 14, .style = {.family = 0, .weight = 400, .slant = UI_TEXT_ITALIC, .size = 18, .color = INK}, .set = UI_TEXT_SET_SPAN_ALL},
    {.start = 15, .end = 19, .style = {.family = 0, .weight = 700, .size = 18, .color = INK}, .set = UI_TEXT_SET_SPAN_ALL},
    {.start = 20, .end = 25, .style = {.family = 0, .weight = 900, .size = 18, .color = INK}, .set = UI_TEXT_SET_SPAN_ALL},
    {.start = 27, .end = 38, .style = {.family = 0, .weight = 700, .slant = UI_TEXT_ITALIC, .size = 18, .color = INK}, .set = UI_TEXT_SET_SPAN_ALL},
};

static const golden_case cases[] = {
    {"latin",
     "The quick brown fox jumps over the lazy dog. Text at fractional scales should look as "
     "crisp as at 1x, with whole-pixel baselines.",
     380, 0, NULL, 0, UI_TEXT_ALIGN_START, 0},
    {"arabic_mixed",
     "\xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9 Arabic with 1234 numbers, "
     "\xD8\xA7\xD9\x84\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85 \xD8\xB9\xD9\x84\xD9\x8A\xD9\x83\xD9\x85 "
     "2026 and English words \xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7.",
     380, 0, NULL, 0, UI_TEXT_ALIGN_START, 0},
    {"hebrew",
     "\xD7\xA2\xD7\x91\xD7\xA8\xD7\x99\xD7\xAA \xD7\xA2\xD7\x9D \xD7\xA4\xD7\x99\xD7\xA1\xD7\x95"
     "\xD7\xA7: \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D, \xD7\xA2\xD7\x95\xD7\x9C\xD7\x9D! "
     "(\xD7\x91\xD7\x93\xD7\x99\xD7\xA7\xD7\x94) \xE2\x80\x94 123.",
     380, 0, NULL, 0, UI_TEXT_ALIGN_START, 0},
    {"cjk",
     "\xE6\x96\x87\xE5\xAD\x97\xE6\x8E\x92\xE7\x89\x88\xE6\x98\xAF\xE4\xB8\x80\xE9\x97\xA8"
     "\xE8\x89\xBA\xE6\x9C\xAF\xE3\x80\x82\xE6\xB1\x89\xE5\xAD\x97\xE3\x80\x81\xE5\x81\x87"
     "\xE5\x90\x8D\xE5\x92\x8C\xE6\xA0\x87\xE7\x82\xB9\xE7\xAC\xA6\xE5\x8F\xB7\xE9\x9C\x80"
     "\xE8\xA6\x81\xE6\xAD\xA3\xE7\xA1\xAE\xE5\x9C\xB0\xE6\x8D\xA2\xE8\xA1\x8C\xEF\xBC\x8C"
     "\xE8\xA1\x8C\xE9\xA6\x96\xE4\xB8\x8D\xE8\x83\xBD\xE5\x87\xBA\xE7\x8E\xB0\xE5\x8F\xA5"
     "\xE5\x8F\xB7\xE6\x88\x96\xE9\x80\x97\xE5\x8F\xB7\xE3\x80\x82",
     300, 0, NULL, 0, UI_TEXT_ALIGN_START, 0},
    {"devanagari",
     "\xE0\xA4\xB9\xE0\xA4\xBF\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA6\xE0\xA5\x80 \xE0\xA4\xAA"
     "\xE0\xA4\xBE\xE0\xA4\xA0: \xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xB7\xE0\xA4\xA4\xE0\xA5\x8D"
     "\xE0\xA4\xB0\xE0\xA4\xBF\xE0\xA4\xAF, \xE0\xA4\xA6\xE0\xA5\x8D\xE0\xA4\xB5\xE0\xA4\xBE"
     "\xE0\xA4\xB0\xE0\xA4\xBE, \xE0\xA4\xB6\xE0\xA5\x8D\xE0\xA4\xB0\xE0\xA5\x80\xE0\xA5\xA4",
     380, 0, NULL, 0, UI_TEXT_ALIGN_START, 0},
    {"ellipsis",
     "Max lines cut this paragraph after the second line, and the last visible line ends with "
     "an ellipsis instead of the rest of the sentence.",
     300, 2, NULL, 0, UI_TEXT_ALIGN_START, 0},
    {"emergency",
     "Pneumonoultramicroscopicsilicovolcanoconiosis",
     120, 0, NULL, 0, UI_TEXT_ALIGN_START, 0},
    {"styles", "Regular italic bold black, bold italic.", 380, 0, style_spans,
     sizeof style_spans / sizeof *style_spans, UI_TEXT_ALIGN_START, 0},
    {"controls",
     "Name:\tAxe\nScripts:\t5\x01\r\nTabs stop every 8 spaces; controls draw nothing.",
     380, 0, NULL, 0, UI_TEXT_ALIGN_START, 0},
    {"justify",
     "Justified text stretches the spaces of every line but the last, so both edges are "
     "flush. Lines here are 1.5 times the font size.",
     380, 0, NULL, 0, UI_TEXT_ALIGN_JUSTIFY, 1.5f},
    {"center",
     "Centered lines with \xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9 "
     "keep a fixed line height: the Arabic font's tall metrics no longer push lines apart.",
     380, 0, NULL, 0, UI_TEXT_ALIGN_CENTER, 1.4f},
};

static const uint32_t scales[] = {120, 150, 180};

typedef struct text_logic {
    const golden_case *c;
    ui_handle_allocator *alloc;
    bool created;
} text_logic;

static ui_result on_events(void *user, const void *events, size_t size, ui_cmd_writer *cmds) {
    text_logic *l = user;
    ui_event_reader rd;
    ui_result r = ui_event_reader_init(&rd, events, size);
    if (r != UI_OK) return r;
    ui_event ev;
    while (ui_event_next(&rd, &ev) == UI_OK) {
        if (ev.type != UI_EVENT_SURFACE_RESIZED || l->created) continue;
        l->created = true;
        ui_handle node;
        if ((r = ui_handle_alloc(l->alloc, &node)) != UI_OK) return r;
        static const char *const families[] = {"Noto Sans"};
        ui_text_desc text = {.set = UI_TEXT_SET_ALL,
            .text = l->c->text,
            .length = (uint32_t)strlen(l->c->text),
            .style = {.family = 0, .weight = 400, .size = 18, .color = {0.08f, 0.08f, 0.1f, 1}},
            .families = families,
            .family_count = 1,
            .max_lines = l->c->max_lines,
            .spans = l->c->spans,
            .span_count = l->c->span_count,
            .align = l->c->align,
            .line_height = l->c->line_height,
        };
        if ((r = ui_cmd_create_node(cmds, node, ev.surface)) != UI_OK ||
            (r = ui_cmd_set_rect(cmds, node, 8, 8, l->c->width, HEIGHT - 16)) != UI_OK ||
            (r = ui_cmd_set_color(cmds, node, 1, 1, 1, 1)) != UI_OK ||
            (r = ui_cmd_set_text(cmds, node, &text)) != UI_OK)
            return r;
    }
    return UI_OK;
}

static bool render(const golden_case *c, uint32_t scale_120, ui_scheduler_kind scheduler,
                   ui_image *out) {
    text_logic logic = {.c = c};
    if (ui_handle_allocator_create(&logic.alloc) != UI_OK) return false;
    ui_app_desc desc = {
        .scheduler = scheduler,
        .gpu = UI_GPU_SOFTWARE,
        .surface = {.kind = UI_SURFACE_HEADLESS, .width = WIDTH, .height = HEIGHT,
                    .scale_120 = scale_120},
        .logic = {.user = &logic, .on_events = on_events},
        .start_paused = true,
        .text = {.font_files = font_files, .font_file_count = UI_ARRAY_LEN(font_files)},
    };
    ui_app *app;
    ui_result r = ui_app_create(&desc, &app);
    bool ok = r == UI_OK;
    if (!ok) fprintf(stderr, "ui_app_create: %s\n", ui_result_str(r));
    // Frame 1 announces the surface; logic answers; frame 2 draws the text.
    ok = ok && ui_app_frame(app) == UI_OK && ui_app_frame(app) == UI_OK &&
         ui_surface_capture(app, ui_app_main_surface(app), out) == UI_OK;
    if (ok) {
        ui_app_stats st;
        ui_app_get_stats(app, &st);
        CHECK_EQ_INT(st.commands_dropped, 0);
        CHECK(st.glyphs_rasterized > 0);
        CHECK_EQ_INT(st.glyphs_rejected, 0);
    }
    if (r == UI_OK) ui_app_destroy(app);
    ui_handle_allocator_destroy(logic.alloc);
    return ok;
}

static void check_case(const golden_case *c, uint32_t scale_120) {
    ui_image single = {0}, threaded = {0};
    bool ok = render(c, scale_120, UI_SCHEDULER_SINGLE, &single) &&
              render(c, scale_120, UI_SCHEDULER_THREADED, &threaded);
    CHECK(ok);
    if (ok) {
        char name[128];
        snprintf(name, sizeof name, "%s@%u", c->name, scale_120 * 100 / 120);
        golden_check(name, &single, &threaded);
    }
    ui_image_free(&single);
    ui_image_free(&threaded);
}

static void test_goldens(void) {
    for (size_t i = 0; i < UI_ARRAY_LEN(cases); i++) {
        for (size_t s = 0; s < UI_ARRAY_LEN(scales); s++) check_case(&cases[i], scales[s]);
    }
}

static void test_qoi_roundtrip(void) {
    ui_rgba_image img = {37, 11, malloc(37 * 11 * 4)};
    for (uint32_t i = 0; i < 37 * 11 * 4; i++)
        img.pixels[i] = (uint8_t)((i % 7 < 3) ? 200 : (i * 31 + i / 5) % 256);
    uint8_t *q;
    size_t n;
    CHECK(ui_qoi_encode(&img, &q, &n) == UI_OK);
    ui_rgba_image back = {0};
    CHECK(ui_qoi_decode(q, n, &back) == UI_OK);
    CHECK(back.width == 37 && back.height == 11);
    CHECK(back.pixels && !memcmp(back.pixels, img.pixels, 37 * 11 * 4));
    ui_rgba_image truncated = {0};
    CHECK(ui_qoi_decode(q, 10, &truncated) == UI_ERR_MALFORMED);
    CHECK(ui_qoi_decode(q, n - 9, &truncated) == UI_ERR_MALFORMED);
    free(q);
    free(back.pixels);
    free(img.pixels);
}

int main(void) {
    RUN(test_qoi_roundtrip);
    RUN(test_goldens);
    return test_failures != 0;
}
