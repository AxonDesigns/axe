// A lost GPU device (docs/architecture.md, "Rendering"): rendering stops, the loss is reported
// once, and the app stays up. Frames keep finishing, logic keeps running and commands keep
// applying, including ones that need the GPU, and shutting down is safe.
//
// The device really is lost as far as wgpu is concerned (ui_render_test_lose_device), so every
// wgpu call that would panic on a lost device stays armed. Runs under both schedulers.
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/app_internal.h"
#include "test.h"
#include "ui/ui.h"

#define WIDTH 200u
#define HEIGHT 100u

typedef enum when_lost {
    NEVER,
    BETWEEN_FRAMES,  // from the main thread, while nothing is drawing
    DURING_A_FRAME,  // from the render thread, with the target acquired
    BEFORE_SHUTDOWN, // no frame sees it
} when_lost;

typedef struct harness {
    ui_app *app;
    ui_handle_allocator *handles;
    ui_cmd_writer *w;
    ui_handle node;
    _Atomic int frames_seen;     // frame_committed events reaching logic
    _Atomic int losses_reported;
    _Atomic bool lose_in_frame;  // the next acquired target loses the device
} harness;

static ui_result count_events(void *user, const void *events, size_t size, ui_cmd_writer *cmds) {
    (void)cmds;
    harness *h = user;
    ui_event_reader r;
    ui_result res = ui_event_reader_init(&r, events, size);
    ui_event ev;
    while (res == UI_OK && (res = ui_event_next(&r, &ev)) == UI_OK) {
        if (ev.type == UI_EVENT_FRAME_COMMITTED) atomic_fetch_add(&h->frames_seen, 1);
    }
    return res == UI_ERR_END ? UI_OK : res;
}

static void on_trace(void *user, const ui_trace_event *ev) {
    harness *h = user;
    if (ev->kind == UI_TRACE_DEVICE_LOST) atomic_fetch_add(&h->losses_reported, 1);
    if (ev->kind == UI_TRACE_PHASE_END && ev->phase == UI_PHASE_ACQUIRE &&
        atomic_exchange(&h->lose_in_frame, false))
        ui_render_test_lose_device(h->app->render);
}

static bool send(harness *h) {
    uint8_t *bytes;
    size_t size;
    if (ui_cmd_writer_take(h->w, &bytes, &size) != UI_OK) return false;
    ui_msg msg = {.type = UI_MSG_CMD_BUFFER, .ptr = bytes, .size = size};
    if (ui_queue_push(&h->app->core.from_logic, &msg) == UI_OK) return true;
    free(bytes);
    return false;
}

// New text at a new size means new glyphs, so an atlas update for the render actor.
static bool set_text(harness *h, float size) {
    static const char text[] = "Lost";
    static const char *const families[] = {"Noto Sans"};
    ui_text_desc d = {.set = UI_TEXT_SET_ALL,
        .text = text, .length = sizeof text - 1,
        .style = {.family = 0, .weight = 400, .size = size, .color = {0, 0, 0, 1}},
        .families = families, .family_count = 1,
    };
    return ui_cmd_set_text(h->w, h->node, &d) == UI_OK && send(h);
}

// The frame after a changed color draws a white or black pixel at (10, 10), proving the display
// list reached the GPU.
static bool set_color(harness *h, float v) {
    return ui_cmd_set_color(h->w, h->node, v, v, v, 1) == UI_OK && send(h);
}

static void run(ui_scheduler_kind scheduler, when_lost when) {
    static const char *const fonts[] = {UI_TEST_FONTS "/NotoSans-VF.ttf"};
    harness h = {0};
    ui_app_desc desc = {
        .scheduler = scheduler,
        .gpu = UI_GPU_SOFTWARE,
        .surface = {.kind = UI_SURFACE_HEADLESS, .width = WIDTH, .height = HEIGHT},
        .logic = {.user = &h, .on_events = count_events},
        .start_paused = true,
        .text = {.font_files = fonts, .font_file_count = 1},
    };
    if (ui_app_create(&desc, &h.app) != UI_OK) {
        fprintf(stderr, "ui_app_create failed\n");
        test_failures++;
        return;
    }
    uint32_t hook;
    ui_result hr = ui_hooks_subscribe(h.app, on_trace, &h, &hook);
    if (hr == UI_ERR_UNSUPPORTED) {
        fprintf(stderr, "  skipped: built without hooks\n");
        ui_app_destroy(h.app);
        return;
    }
    CHECK(hr == UI_OK);
    CHECK(ui_handle_allocator_create(&h.handles) == UI_OK);
    CHECK(ui_cmd_writer_create(&h.w) == UI_OK);
    CHECK(ui_handle_alloc(h.handles, &h.node) == UI_OK);
    ui_cmd_create_node(h.w, h.node, ui_app_main_surface(h.app));
    ui_cmd_set_rect(h.w, h.node, 0, 0, WIDTH, HEIGHT);
    CHECK(set_color(&h, 1));
    CHECK(ui_app_frame(h.app) == UI_OK);

    // It draws before the loss.
    ui_image img = {0};
    CHECK(ui_surface_capture(h.app, ui_app_main_surface(h.app), &img) == UI_OK);
    if (img.pixels) {
        CHECK(img.pixels[(size_t)10 * img.stride + 40] > 200);
        ui_image_free(&img);
    }
    CHECK_EQ_INT(atomic_load(&h.losses_reported), 0);

    if (when == BETWEEN_FRAMES || when == BEFORE_SHUTDOWN)
        ui_render_test_lose_device(h.app->render);
    if (when == DURING_A_FRAME) {
        atomic_store(&h.lose_in_frame, true);
        CHECK(set_color(&h, 0.5f));
        CHECK(ui_app_frame(h.app) == UI_OK); // this one finishes undrawn
    }

    if (when == NEVER || when == BETWEEN_FRAMES || when == DURING_A_FRAME) {
        ui_app_stats before, after;
        ui_app_get_stats(h.app, &before);
        int seen = atomic_load(&h.frames_seen);
        for (int i = 0; i < 4; i++) {
            CHECK(set_color(&h, (float)(i % 2)));
            CHECK(ui_app_frame(h.app) == UI_OK);
        }
        CHECK(set_text(&h, 20));
        CHECK(ui_app_frame(h.app) == UI_OK);
        ui_app_get_stats(h.app, &after);
        CHECK_EQ_INT(after.frames_committed, before.frames_committed + 5);
        CHECK_EQ_INT(after.commands_dropped, before.commands_dropped);
        CHECK_EQ_INT(atomic_load(&h.frames_seen), seen + 5);
        CHECK(after.atlas_updates > before.atlas_updates); // the core still sent one
        ui_result captured = ui_surface_capture(h.app, ui_app_main_surface(h.app), &img);
        CHECK_EQ_INT(captured, when == NEVER ? UI_OK : UI_ERR_GPU);
        if (img.pixels) ui_image_free(&img);
        CHECK_EQ_INT(atomic_load(&h.losses_reported), when == NEVER ? 0 : 1);
    }

    ui_hooks_unsubscribe(h.app, hook);
    ui_cmd_writer_destroy(h.w);
    ui_handle_allocator_destroy(h.handles);
    ui_app_destroy(h.app);
}

static void test_none_single(void) { run(UI_SCHEDULER_SINGLE, NEVER); }
static void test_none_threaded(void) { run(UI_SCHEDULER_THREADED, NEVER); }
static void test_between_frames_single(void) { run(UI_SCHEDULER_SINGLE, BETWEEN_FRAMES); }
static void test_between_frames_threaded(void) { run(UI_SCHEDULER_THREADED, BETWEEN_FRAMES); }
static void test_during_a_frame_single(void) { run(UI_SCHEDULER_SINGLE, DURING_A_FRAME); }
static void test_during_a_frame_threaded(void) { run(UI_SCHEDULER_THREADED, DURING_A_FRAME); }
static void test_before_shutdown_single(void) { run(UI_SCHEDULER_SINGLE, BEFORE_SHUTDOWN); }
static void test_before_shutdown_threaded(void) { run(UI_SCHEDULER_THREADED, BEFORE_SHUTDOWN); }

int main(void) {
    RUN(test_none_single);
    RUN(test_none_threaded);
    RUN(test_between_frames_single);
    RUN(test_between_frames_threaded);
    RUN(test_during_a_frame_single);
    RUN(test_during_a_frame_threaded);
    RUN(test_before_shutdown_single);
    RUN(test_before_shutdown_threaded);
    return test_failures ? 1 : 0;
}
