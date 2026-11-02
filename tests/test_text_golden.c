// Golden images of text rendered headless with the bundled fonts, at scales 1.0, 1.25 and
// 1.5, under both schedulers. The two schedulers must produce byte-identical images; each
// must match the stored golden (tests/goldens/*.qoi, generated under lavapipe) within a
// small tolerance that absorbs sRGB blending differences between driver versions:
// at most GOLDEN_MAX_BAD of the pixels may differ by more than GOLDEN_CHANNEL_TOL per channel.
//
// UI_UPDATE_GOLDENS=1 rewrites the goldens. On a mismatch the actual image, the golden and
// a diff are written as PNGs next to the test binary.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image/image_codec.h"
#include "runtime/common.h"
#include "test.h"
#include "ui/ui.h"

#define GOLDEN_CHANNEL_TOL 2
#define GOLDEN_MAX_BAD 0.0005 // fraction of pixels
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
} golden_case;

static const golden_case cases[] = {
    {"latin",
     "The quick brown fox jumps over the lazy dog. Text at fractional scales should look as "
     "crisp as at 1x, with whole-pixel baselines.",
     380, 0},
    {"arabic_mixed",
     "\xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9 Arabic with 1234 numbers, "
     "\xD8\xA7\xD9\x84\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85 \xD8\xB9\xD9\x84\xD9\x8A\xD9\x83\xD9\x85 "
     "2026 and English words \xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7.",
     380, 0},
    {"hebrew",
     "\xD7\xA2\xD7\x91\xD7\xA8\xD7\x99\xD7\xAA \xD7\xA2\xD7\x9D \xD7\xA4\xD7\x99\xD7\xA1\xD7\x95"
     "\xD7\xA7: \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D, \xD7\xA2\xD7\x95\xD7\x9C\xD7\x9D! "
     "(\xD7\x91\xD7\x93\xD7\x99\xD7\xA7\xD7\x94) \xE2\x80\x94 123.",
     380, 0},
    {"cjk",
     "\xE6\x96\x87\xE5\xAD\x97\xE6\x8E\x92\xE7\x89\x88\xE6\x98\xAF\xE4\xB8\x80\xE9\x97\xA8"
     "\xE8\x89\xBA\xE6\x9C\xAF\xE3\x80\x82\xE6\xB1\x89\xE5\xAD\x97\xE3\x80\x81\xE5\x81\x87"
     "\xE5\x90\x8D\xE5\x92\x8C\xE6\xA0\x87\xE7\x82\xB9\xE7\xAC\xA6\xE5\x8F\xB7\xE9\x9C\x80"
     "\xE8\xA6\x81\xE6\xAD\xA3\xE7\xA1\xAE\xE5\x9C\xB0\xE6\x8D\xA2\xE8\xA1\x8C\xEF\xBC\x8C"
     "\xE8\xA1\x8C\xE9\xA6\x96\xE4\xB8\x8D\xE8\x83\xBD\xE5\x87\xBA\xE7\x8E\xB0\xE5\x8F\xA5"
     "\xE5\x8F\xB7\xE6\x88\x96\xE9\x80\x97\xE5\x8F\xB7\xE3\x80\x82",
     300, 0},
    {"devanagari",
     "\xE0\xA4\xB9\xE0\xA4\xBF\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA6\xE0\xA5\x80 \xE0\xA4\xAA"
     "\xE0\xA4\xBE\xE0\xA4\xA0: \xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xB7\xE0\xA4\xA4\xE0\xA5\x8D"
     "\xE0\xA4\xB0\xE0\xA4\xBF\xE0\xA4\xAF, \xE0\xA4\xA6\xE0\xA5\x8D\xE0\xA4\xB5\xE0\xA4\xBE"
     "\xE0\xA4\xB0\xE0\xA4\xBE, \xE0\xA4\xB6\xE0\xA5\x8D\xE0\xA4\xB0\xE0\xA5\x80\xE0\xA5\xA4",
     380, 0},
    {"ellipsis",
     "Max lines cut this paragraph after the second line, and the last visible line ends with "
     "an ellipsis instead of the rest of the sentence.",
     300, 2},
    {"emergency",
     "Pneumonoultramicroscopicsilicovolcanoconiosis",
     120, 0},
    {"controls",
     "Name:\tAxe\nScripts:\t5\x01\r\nTabs stop every 8 spaces; controls draw nothing.",
     380, 0},
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
        ui_text_desc text = {
            .text = l->c->text,
            .length = (uint32_t)strlen(l->c->text),
            .style = {.family = 0, .weight = 400, .size = 18, .color = {0.08f, 0.08f, 0.1f, 1}},
            .families = families,
            .family_count = 1,
            .max_lines = l->c->max_lines,
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

static void path_for(char *buf, size_t cap, const char *dir, const golden_case *c,
                     uint32_t scale_120, const char *suffix) {
    snprintf(buf, cap, "%s/%s@%u%s", dir, c->name, scale_120 * 100 / 120, suffix);
}

static void write_diff(const ui_rgba_image *a, const ui_rgba_image *b, const char *path) {
    ui_rgba_image d = {a->width, a->height, malloc((size_t)a->width * a->height * 4)};
    if (!d.pixels) return;
    for (size_t i = 0; i < (size_t)a->width * a->height; i++) {
        int m = 0;
        for (int k = 0; k < 3; k++) {
            int v = abs(a->pixels[i * 4 + k] - b->pixels[i * 4 + k]);
            if (v > m) m = v;
        }
        uint8_t v = (uint8_t)(m > GOLDEN_CHANNEL_TOL ? 255 : m * 60);
        d.pixels[i * 4] = v;
        d.pixels[i * 4 + 1] = (uint8_t)(m > GOLDEN_CHANNEL_TOL ? 0 : v);
        d.pixels[i * 4 + 2] = (uint8_t)(m > GOLDEN_CHANNEL_TOL ? 0 : v);
        d.pixels[i * 4 + 3] = 255;
    }
    ui_png_write(&d, path);
    free(d.pixels);
}

static void check_case(const golden_case *c, uint32_t scale_120) {
    ui_image single = {0}, threaded = {0};
    bool ok = render(c, scale_120, UI_SCHEDULER_SINGLE, &single) &&
              render(c, scale_120, UI_SCHEDULER_THREADED, &threaded);
    CHECK(ok);
    if (!ok) goto done;
    CHECK(single.width == threaded.width && single.height == threaded.height);
    if (memcmp(single.pixels, threaded.pixels, (size_t)single.stride * single.height)) {
        fprintf(stderr, "  %s@%u: schedulers differ\n", c->name, scale_120);
        test_failures++;
    }
    ui_rgba_image actual = {single.width, single.height, single.pixels};
    char golden[512], out[512];
    path_for(golden, sizeof golden, UI_TEST_GOLDENS, c, scale_120, ".qoi");
    if (ui_env_flag("UI_UPDATE_GOLDENS")) {
        uint8_t *q;
        size_t qn;
        CHECK(ui_qoi_encode(&actual, &q, &qn) == UI_OK);
        CHECK(ui_file_write(golden, q, qn) == UI_OK);
        free(q);
        fprintf(stderr, "  wrote %s\n", golden);
        goto done;
    }
    uint8_t *data;
    size_t size;
    ui_rgba_image expected = {0};
    if (ui_file_read(golden, &data, &size) != UI_OK) {
        fprintf(stderr, "  missing golden %s (run with UI_UPDATE_GOLDENS=1)\n", golden);
        test_failures++;
        goto done;
    }
    ui_result dr = ui_qoi_decode(data, size, &expected);
    free(data);
    CHECK(dr == UI_OK);
    if (dr != UI_OK) goto done;
    bool same_size = expected.width == actual.width && expected.height == actual.height;
    size_t bad = 0, pixels = (size_t)actual.width * actual.height;
    for (size_t i = 0; same_size && i < pixels; i++) {
        for (int k = 0; k < 4; k++) {
            if (abs(actual.pixels[i * 4 + k] - expected.pixels[i * 4 + k]) > GOLDEN_CHANNEL_TOL) {
                bad++;
                break;
            }
        }
    }
    if (!same_size || (double)bad > GOLDEN_MAX_BAD * (double)pixels) {
        fprintf(stderr, "  %s@%u: %zu of %zu pixels differ from the golden%s\n", c->name,
                scale_120, bad, pixels, same_size ? "" : " (size differs)");
        test_failures++;
        path_for(out, sizeof out, UI_TEST_OUT, c, scale_120, "-actual.png");
        ui_png_write(&actual, out);
        path_for(out, sizeof out, UI_TEST_OUT, c, scale_120, "-expected.png");
        ui_png_write(&expected, out);
        if (same_size) {
            path_for(out, sizeof out, UI_TEST_OUT, c, scale_120, "-diff.png");
            write_diff(&actual, &expected, out);
        }
    }
    free(expected.pixels);
done:
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
