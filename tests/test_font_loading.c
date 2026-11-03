// Loading fonts at runtime: load_font registers a file, answers with font_loaded, and text
// already on screen is shaped again with the new font (here Hebrew, which the app could
// only draw as .notdef before). The file is read and the text reshaped on workers, so the
// frame that applies load_font shows the new font without the core shaping anything.
#include <stdatomic.h>
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
            ui_text_desc t = {.text = HEBREW, .length = sizeof HEBREW - 1,
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

static void test_load_font_single(void) { load_font_with(UI_SCHEDULER_SINGLE); }
static void test_load_font_threaded(void) { load_font_with(UI_SCHEDULER_THREADED); }

int main(void) {
    RUN(test_load_font_single);
    RUN(test_load_font_threaded);
    return test_failures != 0;
}
