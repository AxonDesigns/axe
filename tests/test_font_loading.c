// Loading fonts at runtime: load_font registers a file, answers with font_loaded, and text
// already on screen is shaped again with the new font (here Hebrew, which the app could
// only draw as .notdef before). The file is read and the text reshaped on workers, so the
// frame that applies load_font shows the new font without the core shaping anything.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/app_internal.h"
#include "test.h"
#include "ui/ui.h"

#define FONT(name) UI_TEST_FONTS "/" name

static const char *const latin_only[] = {FONT("NotoSans-VF.ttf")};
static const char HEBREW[] = "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D \xD7\xA2\xD7\x95\xD7\x9C\xD7\x9D";

typedef struct font_logic {
    ui_handle_allocator *alloc;
    ui_handle node;
    _Atomic int loaded_ok, loaded_failed;
} font_logic;

static ui_result on_events(void *user, const void *events, size_t size, ui_cmd_writer *cmds) {
    font_logic *l = user;
    ui_event_reader rd;
    ui_result r = ui_event_reader_init(&rd, events, size);
    ui_event ev;
    while (r == UI_OK && ui_event_next(&rd, &ev) == UI_OK) {
        if (ev.type == UI_EVENT_SURFACE_RESIZED && ui_handle_is_null(l->node)) {
            ui_handle_alloc(l->alloc, &l->node);
            static const char *const fam[] = {"Noto Sans"};
            ui_text_desc t = {.set = UI_TEXT_SET_ALL, .text = HEBREW, .length = sizeof HEBREW - 1,
                              .style = {.family = 0, .weight = 400, .size = 20,
                                        .color = {0, 0, 0, 1}},
                              .families = fam, .family_count = 1};
            ui_cmd_create_node(cmds, l->node, ev.surface);
            ui_cmd_set_rect(cmds, l->node, 0, 0, 200, 60);
            ui_cmd_set_color(cmds, l->node, 1, 1, 1, 1);
            ui_cmd_set_text(cmds, l->node, &t);
        } else if (ev.type == UI_EVENT_FONT_LOADED) {
            if (ev.font_loaded.result == UI_OK && ev.font_loaded.id == 7) atomic_fetch_add(&l->loaded_ok, 1);
            if (ev.font_loaded.result == UI_ERR_IO && ev.font_loaded.id == 8)
                atomic_fetch_add(&l->loaded_failed, 1);
        }
    }
    return UI_OK;
}

static uint32_t first_run_face(ui_app *app, ui_handle node) {
    ui_paragraph *p = app->core.scene.nodes[ui_handle_slot(node)].text;
    return p && p->shaped_valid && p->shaped.run_count ? p->shaped.runs[0].font.face : UINT32_MAX;
}

static void load_font_with(ui_scheduler_kind scheduler) {
    font_logic logic = {0};
    ui_handle_allocator_create(&logic.alloc);
    atomic_init(&logic.loaded_ok, 0);
    atomic_init(&logic.loaded_failed, 0);
    ui_app_desc desc = {
        .scheduler = scheduler,
        .gpu = UI_GPU_SOFTWARE,
        .surface = {.kind = UI_SURFACE_HEADLESS, .width = 200, .height = 60},
        .logic = {.user = &logic, .on_events = on_events},
        .start_paused = true,
        .text = {.font_files = latin_only, .font_file_count = 1},
    };
    ui_app *app;
    CHECK(ui_app_create(&desc, &app) == UI_OK);
    CHECK(ui_app_frame(app) == UI_OK && ui_app_frame(app) == UI_OK);
    CHECK_EQ_INT(first_run_face(app, logic.node), 0); // only Noto Sans: .notdef boxes
    ui_image before;
    CHECK(ui_surface_capture(app, ui_app_main_surface(app), &before) == UI_OK);

    ui_cmd_writer *w;
    ui_cmd_writer_create(&w);
    CHECK(ui_cmd_load_font(w, 7, FONT("NotoSansHebrew-VF.ttf")) == UI_OK);
    CHECK(ui_cmd_load_font(w, 8, FONT("missing.ttf")) == UI_OK);
    CHECK(ui_cmd_load_font(w, 9, "") == UI_ERR_INVALID_ARGUMENT);
    uint8_t *bytes;
    size_t size;
    ui_cmd_writer_take(w, &bytes, &size);
    ui_msg msg = {.type = UI_MSG_CMD_BUFFER, .ptr = bytes, .size = size};
    CHECK(ui_queue_push(&app->core.from_logic, &msg) == UI_OK);
    CHECK(ui_app_frame(app) == UI_OK); // applies load_font and redraws with the new font
    CHECK_EQ_INT(atomic_load(&logic.loaded_ok), 1);
    CHECK_EQ_INT(atomic_load(&logic.loaded_failed), 1);
    CHECK_EQ_INT(first_run_face(app, logic.node), 1); // the text now falls back to Hebrew
    ui_image after;
    CHECK(ui_surface_capture(app, ui_app_main_surface(app), &after) == UI_OK);
    CHECK(memcmp(before.pixels, after.pixels, (size_t)before.stride * before.height) != 0);
    ui_app_stats stats;
    ui_app_get_stats(app, &stats);
    CHECK_EQ_INT(stats.text_shaped_on_core, 0);
    CHECK_EQ_INT(stats.glyphs_rasterized_on_core, 0);
    ui_image_free(&before);
    ui_image_free(&after);
    ui_cmd_writer_destroy(w);
    ui_app_destroy(app);
    ui_handle_allocator_destroy(logic.alloc);
}

// A buffer that loads a font and sets text: its text was shaped before the font registers,
// so registering sends it back to the workers, and the head step must not read it while
// they shape it (once it did, and freed sources were read). Four rounds, each loading a
// new font with many paragraphs in the same buffer, under the threaded scheduler; their
// sizes come from styles, so the sources shaped are derived ones the workers free when
// they replace them (ASan catches the read when the timing exposes it). Every paragraph
// ends up shaped with the fonts as they are.
static const char *const SCRIPTS[][2] = {
    {"NotoSansHebrew-VF.ttf", "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D"},
    {"NotoSansArabic-VF.ttf", "\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85"},
    {"NotoSansDevanagari-VF.ttf", "\xE0\xA4\xA8\xE0\xA4\xAE\xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\xA4\xE0\xA5\x87"},
    {"NotoSansSC-Subset.ttf", "\xE6\x96\x87\xE5\xAD\x97"},
};
// The face each round's text uses: its new font, except Devanagari, which Noto Sans (face
// 0) covers itself. Registering any font still sends the round's text back to the workers.
static const uint32_t FACE[] = {1, 2, 0, 4};
enum { PARAGRAPHS = 48 };

static void load_font_with_text(void) {
    font_logic logic = {0}; // creates nothing: the buffers below do
    logic.node = (ui_handle){1, 1};
    ui_app_desc desc = {
        .scheduler = UI_SCHEDULER_THREADED,
        .gpu = UI_GPU_SOFTWARE,
        .surface = {.kind = UI_SURFACE_HEADLESS, .width = 200, .height = 60},
        .logic = {.user = &logic, .on_events = on_events},
        .start_paused = true,
        .text = {.font_files = latin_only, .font_file_count = 1},
    };
    ui_app *app;
    CHECK(ui_app_create(&desc, &app) == UI_OK);
    CHECK(ui_app_frame(app) == UI_OK);
    ui_handle surface = ui_app_main_surface(app);
    ui_cmd_writer *w;
    ui_cmd_writer_create(&w);
    static const char *const fam[] = {"Noto Sans"};
    for (uint32_t round = 0; round < 4; round++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", UI_TEST_FONTS, SCRIPTS[round][0]);
        CHECK(ui_cmd_load_font(w, 100 + round, path) == UI_OK);
        for (uint32_t i = 0; i < PARAGRAPHS; i++) {
            ui_handle h = {2 + round * PARAGRAPHS + i, 1};
            // The size comes from the node's style, so what the workers shape is a source
            // derived for the node, which the worker frees when it shapes again.
            ui_text_desc t = {.set = UI_TEXT_SET_ALL & ~UI_TEXT_SET_SIZE, .text = SCRIPTS[round][1],
                              .length = (uint32_t)strlen(SCRIPTS[round][1]),
                              .style = {.family = 0, .weight = 400, .size = 12, .color = {0, 0, 0, 1}},
                              .families = fam, .family_count = 1};
            CHECK(ui_cmd_create_node(w, h, surface) == UI_OK);
            CHECK(ui_cmd_set_rect(w, h, 0, 0, 200, 20) == UI_OK);
            ui_style_decl size = {UI_STYLE_FONT_SIZE, ui_px(13 + (float)(i % 5))};
            CHECK(ui_cmd_set_props(w, h, &size, 1) == UI_OK);
            CHECK(ui_cmd_set_text(w, h, &t) == UI_OK);
        }
        uint8_t *bytes;
        size_t size;
        ui_cmd_writer_take(w, &bytes, &size);
        ui_msg msg = {.type = UI_MSG_CMD_BUFFER, .ptr = bytes, .size = size};
        CHECK(ui_queue_push(&app->core.from_logic, &msg) == UI_OK);
        for (int f = 0; f < 3; f++) CHECK(ui_app_frame(app) == UI_OK);
        for (uint32_t i = 0; i < PARAGRAPHS; i++) // shaped with the fonts as they are now
            CHECK_EQ_INT(first_run_face(app, (ui_handle){2 + round * PARAGRAPHS + i, 1}), FACE[round]);
    }
    ui_app_stats stats;
    ui_app_get_stats(app, &stats);
    CHECK_EQ_INT(stats.text_shaped_on_core, 0);
    ui_cmd_writer_destroy(w);
    ui_app_destroy(app);
}

// Several fresh apps: each registers the four fonts again, another chance for the timing.
static void test_load_font_with_text(void) {
    for (int i = 0; i < 6; i++) load_font_with_text();
}

static void test_load_font_single(void) { load_font_with(UI_SCHEDULER_SINGLE); }
static void test_load_font_threaded(void) { load_font_with(UI_SCHEDULER_THREADED); }

int main(void) {
    RUN(test_load_font_single);
    RUN(test_load_font_threaded);
    RUN(test_load_font_with_text);
    return test_failures != 0;
}
