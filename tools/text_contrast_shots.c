// Renders the text-contrast comparison images for docs/text.md: the same sample at several
// coverage-contrast settings, dark on light and light on dark, headless on lavapipe.
//   cmake --build --preset debug --target text_contrast_shots
//   UI_GPU=software build/debug/tools/text_contrast_shots docs/images
// Developer tool only: it is excluded from the default build.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image/image_codec.h"
#include "ui/ui.h"

#define WIDTH 300u
#define HEIGHT 76u

static const char *const fonts[] = {AXE_SOURCE_DIR "/tests/fonts/NotoSans-VF.ttf"};
static const char SAMPLE[] = "Hamburgefonstiv 0123 — the quick brown fox.";

typedef struct shot {
    bool dark;  // light text on a dark background
    ui_handle_allocator *alloc;
    bool done;
} shot;

static ui_result on_events(void *user, const void *events, size_t size, ui_cmd_writer *cmds) {
    shot *s = user;
    ui_event_reader rd;
    ui_result r = ui_event_reader_init(&rd, events, size);
    ui_event ev;
    while (r == UI_OK && ui_event_next(&rd, &ev) == UI_OK) {
        if (ev.type != UI_EVENT_SURFACE_RESIZED || s->done) continue;
        s->done = true;
        float fg = s->dark ? 0.92f : 0.1f, bg = s->dark ? 0.1f : 1.f;
        static const char *const families[] = {"Noto Sans"};
        ui_text_desc t = {.text = SAMPLE, .length = sizeof SAMPLE - 1,
                          .style = {.family = 0, .weight = 400, .size = 13,
                                    .color = {fg, fg, fg, 1}},
                          .families = families, .family_count = 1};
        ui_text_desc t2 = t;
        t2.style.size = 18;
        ui_handle a, b;
        ui_handle_alloc(s->alloc, &a);
        ui_handle_alloc(s->alloc, &b);
        ui_cmd_create_node(cmds, a, ev.surface);
        ui_cmd_set_rect(cmds, a, 0, 0, WIDTH, HEIGHT);
        ui_cmd_set_color(cmds, a, bg, bg, bg, 1);
        ui_cmd_set_text(cmds, a, &t);
        ui_cmd_create_node(cmds, b, a);
        ui_cmd_set_rect(cmds, b, 6, 26, WIDTH - 12, 40);
        ui_cmd_set_text(cmds, b, &t2);
    }
    return UI_OK;
}

static int render(const char *dir, float contrast, bool dark) {
    shot s = {.dark = dark};
    ui_handle_allocator_create(&s.alloc);
    ui_app_desc desc = {
        .scheduler = UI_SCHEDULER_SINGLE,
        .surface = {.kind = UI_SURFACE_HEADLESS, .width = WIDTH, .height = HEIGHT},
        .logic = {.user = &s, .on_events = on_events},
        .start_paused = true,
        .text = {.font_files = fonts, .font_file_count = 1, .contrast = contrast},
    };
    // contrast 0 in the desc means "default"; the environment override can set 0 itself.
    char value[32];
    snprintf(value, sizeof value, "%g", (double)contrast);
    setenv("UI_TEXT_CONTRAST", value, 1);
    ui_app *app;
    if (ui_app_create(&desc, &app) != UI_OK) return 1;
    ui_image img = {0};
    int rc = ui_app_frame(app) || ui_app_frame(app) ||
             ui_surface_capture(app, ui_app_main_surface(app), &img);
    if (!rc) {
        char path[512];
        snprintf(path, sizeof path, "%s/text-contrast-%s-%03d.png", dir, dark ? "dark" : "light",
                 (int)(contrast * 100 + 0.5f));
        ui_rgba_image out = {img.width, img.height, img.pixels};
        rc = ui_png_write(&out, path) != UI_OK;
        if (!rc) printf("%s\n", path);
    }
    ui_image_free(&img);
    ui_app_destroy(app);
    ui_handle_allocator_destroy(s.alloc);
    return rc;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    static const float settings[] = {0.f, 0.5f, 0.9f, 1.f};
    for (size_t i = 0; i < sizeof settings / sizeof *settings; i++) {
        if (render(dir, settings[i], false) || render(dir, settings[i], true)) return 1;
    }
    return 0;
}
