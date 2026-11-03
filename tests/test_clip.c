// Clipping: a node with set_clip draws its text and descendants only inside its rect, and
// what lies entirely outside is culled before rasterization. Pixels are checked on a
// headless surface, against the same scene without the clip.
#include <stdlib.h>
#include <string.h>

#include "runtime/app_internal.h"
#include "test.h"
#include "ui/ui.h"

#define WIDTH 200u
#define HEIGHT 120u
#define FONT(name) UI_TEST_FONTS "/" name

static const char *const font_files[] = {FONT("NotoSans-VF.ttf")};
static const char TEXT[] = "Clipped text continues for several lines below the box, and "
                           "none of it may show outside.";

typedef struct shot {
    ui_image image;
    ui_app_stats stats;
} shot;

static bool render(bool clip, shot *out) {
    ui_app_desc desc = {
        .scheduler = UI_SCHEDULER_SINGLE,
        .gpu = UI_GPU_SOFTWARE,
        .surface = {.kind = UI_SURFACE_HEADLESS, .width = WIDTH, .height = HEIGHT},
        .start_paused = true,
        .text = {.font_files = font_files, .font_file_count = 1},
    };
    ui_app *app;
    if (ui_app_create(&desc, &app) != UI_OK) return false;
    ui_handle_allocator *h;
    ui_cmd_writer *w;
    ui_handle_allocator_create(&h);
    ui_cmd_writer_create(&w);
    ui_handle bg, box, child;
    ui_handle_alloc(h, &bg);
    ui_handle_alloc(h, &box);
    ui_handle_alloc(h, &child);
    static const char *const families[] = {"Noto Sans"};
    ui_text_desc t = {.text = TEXT, .length = sizeof TEXT - 1,
                      .style = {.family = 0, .weight = 400, .size = 16, .color = {0, 0, 0, 1}},
                      .families = families, .family_count = 1};
    ui_cmd_create_node(w, bg, ui_app_main_surface(app));
    ui_cmd_set_rect(w, bg, 0, 0, WIDTH, HEIGHT);
    ui_cmd_set_color(w, bg, 1, 1, 1, 1);
    ui_cmd_create_node(w, box, bg);
    ui_cmd_set_rect(w, box, 10, 10, 100, 30); // the clip: x 10..110, y 10..40
    ui_cmd_set_text(w, box, &t);
    ui_cmd_set_clip(w, box, clip);
    ui_cmd_create_node(w, child, box);
    ui_cmd_set_rect(w, child, 70, 5, 60, 60); // reaches x 140, y 75 without the clip
    ui_cmd_set_color(w, child, 1, 0, 0, 1);
    uint8_t *bytes;
    size_t size;
    ui_cmd_writer_take(w, &bytes, &size);
    ui_msg msg = {.type = UI_MSG_CMD_BUFFER, .ptr = bytes, .size = size};
    bool ok = ui_queue_push(&app->core.from_logic, &msg) == UI_OK && ui_app_frame(app) == UI_OK &&
              ui_surface_capture(app, ui_app_main_surface(app), &out->image) == UI_OK;
    ui_app_get_stats(app, &out->stats);
    ui_cmd_writer_destroy(w);
    ui_handle_allocator_destroy(h);
    ui_app_destroy(app);
    return ok;
}

static bool white(const ui_image *img, uint32_t x, uint32_t y) {
    const uint8_t *p = img->pixels + (size_t)y * img->stride + 4u * x;
    return p[0] == 255 && p[1] == 255 && p[2] == 255;
}

static bool red(const ui_image *img, uint32_t x, uint32_t y) {
    const uint8_t *p = img->pixels + (size_t)y * img->stride + 4u * x;
    return p[0] > 200 && p[1] < 60 && p[2] < 60;
}

// Counts non-white pixels in [x0, x1) x [y0, y1).
static uint32_t ink(const ui_image *img, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    uint32_t n = 0;
    for (uint32_t y = y0; y < y1; y++) {
        for (uint32_t x = x0; x < x1; x++) n += !white(img, x, y);
    }
    return n;
}

static void test_clip(void) {
    shot clipped = {0}, open = {0};
    CHECK(render(true, &clipped));
    CHECK(render(false, &open));
    if (!clipped.image.pixels || !open.image.pixels) return;
    // Without the clip, text runs below the box and the red child sticks out of it.
    CHECK(ink(&open.image, 0, 41, WIDTH, HEIGHT) > 100);
    CHECK(red(&open.image, 120, 30));
    // With it, nothing is drawn outside x 10..110, y 10..40 (the background aside).
    CHECK_EQ_INT(ink(&clipped.image, 0, 40, WIDTH, HEIGHT), 0);
    CHECK_EQ_INT(ink(&clipped.image, 110, 0, WIDTH, HEIGHT), 0);
    CHECK_EQ_INT(ink(&clipped.image, 0, 0, WIDTH, 10), 0);
    // Inside, the text and the red child are drawn.
    CHECK(ink(&clipped.image, 10, 10, 80, 40) > 50);
    CHECK(red(&clipped.image, 100, 30));
    // Lines entirely below the box are culled, never rasterized.
    CHECK(clipped.stats.glyphs_rasterized < open.stats.glyphs_rasterized);
    ui_image_free(&clipped.image);
    ui_image_free(&open.image);
}

static void test_huge_rects(void) {
    // Rects may hold any finite value, so clips can lie far outside the int range (or
    // overflow to infinity when added up): the scissor must still come out in range
    // (UBSan checks the conversions).
    ui_logger quiet = {UI_LOG_ERROR};
    ui_scene s;
    ui_scene_init(&s, &quiet);
    ui_handle surface = {(uint32_t)UI_HANDLE_SURFACE << UI_HANDLE_SLOT_BITS, 1};
    ui_scene_add_root(&s, surface);
    ui_cmd_writer *w;
    ui_cmd_writer_create(&w);
    const float rects[][4] = {{3e21f, -3e21f, 1e30f, 5}, {-3e38f, -3e38f, 3e38f, 3e38f},
                              {3e38f, 3e38f, 3e38f, 3e38f}};
    ui_handle parent = surface;
    for (uint32_t i = 0; i < 3; i++) {
        ui_handle n = {i + 1, 1}, leaf = {i + 10, 1};
        ui_cmd_create_node(w, n, parent);
        ui_cmd_set_rect(w, n, rects[i][0], rects[i][1], rects[i][2], rects[i][3]);
        ui_cmd_set_clip(w, n, true);
        ui_cmd_create_node(w, leaf, n);
        ui_cmd_set_rect(w, leaf, 0, 0, 10, 10);
        ui_cmd_set_color(w, leaf, 1, 0, 0, 1);
        parent = n;
    }
    uint8_t *bytes;
    size_t size;
    ui_cmd_writer_take(w, &bytes, &size);
    CHECK(ui_scene_apply(&s, bytes, size, 0) == UI_OK);
    ui_scene_resolve(&s, 0);
    CHECK_EQ_INT(s.stats.dropped, 0);
    ui_display_list dl = {.surface = surface, .width = WIDTH, .height = HEIGHT,
                          .logical_width = WIDTH, .logical_height = HEIGHT, .scale_120 = 120};
    ui_display_list_reset(&dl);
    CHECK(ui_scene_build(&s, surface, &dl, NULL) == UI_OK);
    for (uint32_t i = 0; i < dl.batch_count; i++)
        CHECK(dl.batches[i].clip.width >= 0 && dl.batches[i].clip.height >= 0);
    ui_display_list_free(&dl);
    free(bytes);
    ui_cmd_writer_destroy(w);
    ui_scene_destroy(&s);
}

int main(void) {
    RUN(test_clip);
    RUN(test_huge_rects);
    return test_failures != 0;
}
