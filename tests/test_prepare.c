// Command buffers are prepared off the core thread and applied whole, in order, once ready
// (core/prepare.h):
//   - threaded: a worker is held inside the preparation of a buffer with a long paragraph.
//     Frames keep committing and a spring keeps moving; neither that buffer nor a later one
//     applies until the worker is released, then both apply in the same frame;
//   - both schedulers: ui_app_frame applies a buffer queued before it, with the text shaped
//     and its glyphs rasterized by workers (none on the core), and the pictures match byte
//     for byte;
//   - an app whose platform fails to start returns the error (font discovery hasn't woken it).
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "core/spring.h"
#include "runtime/app_internal.h"
#include "test.h"
#include "ui/ui.h"

#define WIDTH 320u
#define HEIGHT 120u
#define FROM_X 20.0f
#define TO_X 220.0f
#define STIFFNESS 170.0f
#define DAMPING 18.25f
#define STEP_NS 16000000ull
#define FONT(name) UI_TEST_FONTS "/" name

static const char *const font_files[] = {FONT("NotoSans-VF.ttf"), FONT("NotoSansHebrew-VF.ttf")};

typedef struct client {
    ui_app *app;
    ui_handle_allocator *handles;
    ui_cmd_writer *w;
    ui_handle rect, text;
    // The hold: a worker blocks when it starts preparing buffer `hold_seq`.
    mtx_t lock;
    cnd_t cond;
    uint64_t hold_seq;
    bool held, release;
} client;

static void on_trace(void *user, const ui_trace_event *ev) {
    client *c = user;
    if (ev->kind != UI_TRACE_PREPARE_BEGIN || ev->frame_id != c->hold_seq) return;
    mtx_lock(&c->lock);
    c->held = true;
    cnd_broadcast(&c->cond);
    while (!c->release) cnd_wait(&c->cond, &c->lock);
    mtx_unlock(&c->lock);
}

// Sends a command buffer through the logic -> core channel, as the logic actor would.
static bool send(client *c) {
    uint8_t *bytes;
    size_t size;
    if (ui_cmd_writer_take(c->w, &bytes, &size) != UI_OK) return false;
    ui_msg msg = {.type = UI_MSG_CMD_BUFFER, .ptr = bytes, .size = size};
    if (ui_queue_push(&c->app->core.from_logic, &msg) == UI_OK) return true;
    free(bytes);
    return false;
}

static bool open_app(client *c, ui_scheduler_kind scheduler) {
    memset(c, 0, sizeof *c);
    mtx_init(&c->lock, mtx_plain);
    cnd_init(&c->cond);
    ui_app_desc desc = {
        .scheduler = scheduler,
        .gpu = UI_GPU_SOFTWARE,
        .surface = {.kind = UI_SURFACE_HEADLESS, .width = WIDTH, .height = HEIGHT},
        .start_paused = true,
        .text = {.font_files = font_files, .font_file_count = 2},
        .worker_threads = 2,
    };
    if (ui_app_create(&desc, &c->app) != UI_OK || ui_handle_allocator_create(&c->handles) != UI_OK ||
        ui_cmd_writer_create(&c->w) != UI_OK)
        return false;
    ui_handle_alloc(c->handles, &c->rect);
    ui_handle_alloc(c->handles, &c->text);
    return ui_hooks_subscribe(c->app, on_trace, c, NULL) == UI_OK;
}

static void close_app(client *c) {
    if (c->app) ui_app_destroy(c->app);
    ui_cmd_writer_destroy(c->w);
    ui_handle_allocator_destroy(c->handles);
    cnd_destroy(&c->cond);
    mtx_destroy(&c->lock);
}

// A white square that springs from FROM_X to TO_X.
static bool send_spring(client *c) {
    ui_handle surface = ui_app_main_surface(c->app);
    ui_cmd_create_node(c->w, c->rect, surface);
    ui_cmd_set_rect(c->w, c->rect, FROM_X, 10, 30, 30);
    ui_cmd_set_color(c->w, c->rect, 1, 1, 1, 1);
    float target[4] = {TO_X, 10, 30, 30};
    ui_cmd_start_spring(c->w, c->rect, UI_PROP_RECT, target, STIFFNESS, DAMPING);
    return send(c);
}

// A blue node with a long paragraph in two scripts, all in one buffer.
static bool send_text(client *c) {
    static char text[6000];
    size_t n = 0;
    static const char words[] = "Preparation runs on workers \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D ";
    while (n + sizeof words < sizeof text) {
        memcpy(text + n, words, sizeof words - 1);
        n += sizeof words - 1;
    }
    static const char *const families[] = {"Noto Sans"};
    ui_text_desc d = {
        .text = text, .length = (uint32_t)n,
        .style = {.family = 0, .weight = 400, .size = 13, .color = {1, 1, 1, 1}},
        .families = families, .family_count = 1,
    };
    ui_cmd_create_node(c->w, c->text, ui_app_main_surface(c->app));
    ui_cmd_set_rect(c->w, c->text, 0, 60, WIDTH, 60);
    ui_cmd_set_color(c->w, c->text, 0, 0, 1, 1);
    ui_cmd_set_clip(c->w, c->text, true);
    return ui_cmd_set_text(c->w, c->text, &d) == UI_OK && send(c);
}

static bool capture(client *c, ui_image *img) {
    return ui_surface_capture(c->app, ui_app_main_surface(c->app), img) == UI_OK;
}

static const uint8_t *pixel(const ui_image *img, uint32_t x, uint32_t y) {
    return img->pixels + (size_t)y * img->stride + 4u * x;
}

// Left edge of the square on its middle row (any bright pixel: white, or red later).
static int left_edge(const ui_image *img) {
    for (uint32_t x = 0; x < img->width; x++) {
        if (pixel(img, x, 25)[0] > 128) return (int)x;
    }
    return -1;
}

static void test_held_preparation(void) {
    client c;
    if (!open_app(&c, UI_SCHEDULER_THREADED)) {
        fprintf(stderr, "cannot create the app\n");
        test_failures++;
        close_app(&c);
        return;
    }
    CHECK(send_spring(&c) && ui_app_frame(c.app) == UI_OK); // the spring starts at t = 0
    // Buffer 1 to be prepared holds the text; the next only recolors the square.
    c.hold_seq = 1;
    CHECK(send_text(&c));
    ui_cmd_set_color(c.w, c.rect, 1, 0, 0, 1);
    CHECK(send(&c));

    ui_app_stats stats;
    ui_app_get_stats(c.app, &stats);
    uint64_t committed = stats.frames_committed;
    int previous = (int)FROM_X;
    for (int i = 1; i <= 12; i++) {
        ui_app_clock_step(c.app, STEP_NS);
        CHECK(ui_app_frame_without_logic(c.app) == UI_OK);
        if (i == 1) {
            mtx_lock(&c.lock);
            CHECK(c.held); // the core started preparing on receipt; the worker is in the hook
            mtx_unlock(&c.lock);
        }
        ui_app_get_stats(c.app, &stats);
        CHECK_EQ_INT(stats.frames_committed, committed + (uint64_t)i); // frames keep coming
        ui_image img = {0};
        CHECK(capture(&c, &img));
        if (!img.pixels) break;
        double x, v;
        ui_spring_solve(STIFFNESS, DAMPING, FROM_X - TO_X, 0.0, (double)(i * STEP_NS) * 1e-9, &x, &v);
        int left = left_edge(&img);
        CHECK_NEAR(left, TO_X + x, 1.5);
        if (i <= 6) CHECK(left > previous); // still moving
        previous = left;
        const uint8_t *square = pixel(&img, (uint32_t)left + 15, 25);
        CHECK(square[1] > 200); // still white: the later buffer waits behind the held one
        CHECK(pixel(&img, 300, 110)[2] < 60); // the text node doesn't exist yet
        ui_image_free(&img);
    }
    ui_app_get_stats(c.app, &stats);
    CHECK_EQ_INT(stats.buffers_deferred, 0); // still waiting, not applied

    mtx_lock(&c.lock);
    c.release = true;
    cnd_broadcast(&c.cond);
    mtx_unlock(&c.lock);
    CHECK(ui_app_frame(c.app) == UI_OK); // both buffers apply, in order, in this frame
    ui_image img = {0};
    CHECK(capture(&c, &img));
    if (img.pixels) {
        int left = left_edge(&img);
        const uint8_t *square = pixel(&img, (uint32_t)left + 15, 25);
        CHECK(square[0] > 200 && square[1] < 60); // red
        CHECK(pixel(&img, 300, 110)[2] > 200);   // the blue text node
        ui_image_free(&img);
    }
    ui_app_get_stats(c.app, &stats);
    CHECK_EQ_INT(stats.buffers_deferred, 1);
    CHECK(stats.max_head_wait_ns > 0);
    CHECK_EQ_INT(stats.texts_prepared, 1);
    CHECK(stats.glyphs_prepared > 0);
    CHECK_EQ_INT(stats.text_shaped_on_core, 0);
    CHECK_EQ_INT(stats.glyphs_rasterized_on_core, 0);
    CHECK_EQ_INT(stats.commands_dropped, 0);
    close_app(&c);
}

// ui_app_frame applies what was queued before it under both schedulers, prepared by workers.
static bool render_text(ui_scheduler_kind scheduler, ui_image *out, ui_app_stats *stats) {
    client c;
    bool ok = open_app(&c, scheduler);
    c.hold_seq = UINT64_MAX;
    ok = ok && send_spring(&c) && send_text(&c) && ui_app_frame(c.app) == UI_OK;
    if (ok) ui_app_clock_step(c.app, STEP_NS * 3);
    ok = ok && ui_app_frame(c.app) == UI_OK && capture(&c, out);
    if (ok) ui_app_get_stats(c.app, stats);
    close_app(&c);
    return ok;
}

static void test_schedulers_match(void) {
    ui_image single = {0}, threaded = {0};
    ui_app_stats ss, ts;
    CHECK(render_text(UI_SCHEDULER_SINGLE, &single, &ss));
    CHECK(render_text(UI_SCHEDULER_THREADED, &threaded, &ts));
    if (single.pixels && threaded.pixels) {
        CHECK(single.width == threaded.width && single.height == threaded.height);
        CHECK(!memcmp(single.pixels, threaded.pixels, (size_t)single.stride * single.height));
        CHECK(pixel(&single, 300, 110)[2] > 200); // the text applied in the first frame
    }
    const ui_app_stats *all[] = {&ss, &ts};
    for (int i = 0; i < 2; i++) {
        CHECK_EQ_INT(all[i]->texts_prepared, 1);
        CHECK(all[i]->glyphs_prepared > 0);
        CHECK_EQ_INT(all[i]->text_shaped_on_core, 0);
        CHECK_EQ_INT(all[i]->glyphs_rasterized_on_core, 0);
        CHECK_EQ_INT(all[i]->buffers_deferred, 0);
        CHECK(all[i]->glyphs_rasterized > 0);
    }
    ui_image_free(&single);
    ui_image_free(&threaded);
}

// An app whose platform can't start fails cleanly. Regression: font discovery started as a
// job while the core was created, before the platform; when the platform then failed, the
// job's completion woke the missing platform and crashed.
static void test_failed_start(void) {
    const char *saved = getenv("WAYLAND_DISPLAY");
    char *copy = saved ? strdup(saved) : NULL;
    setenv("WAYLAND_DISPLAY", "axe-test-no-such-display", 1);
    for (int k = 0; k < 2; k++) {
        ui_app_desc desc = {
            .scheduler = k ? UI_SCHEDULER_THREADED : UI_SCHEDULER_SINGLE,
            .gpu = UI_GPU_SOFTWARE,
            .surface = {.kind = UI_SURFACE_WINDOW, .width = WIDTH, .height = HEIGHT},
            .text = {.font_files = font_files, .font_file_count = 1, .system_fonts = true},
        };
        ui_app *app = NULL;
        CHECK(ui_app_create(&desc, &app) == UI_ERR_PLATFORM && !app);
    }
    if (copy) setenv("WAYLAND_DISPLAY", copy, 1);
    else unsetenv("WAYLAND_DISPLAY");
    free(copy);
}

int main(void) {
    RUN(test_held_preparation);
    RUN(test_schedulers_match);
    RUN(test_failed_start);
    return test_failures != 0;
}
