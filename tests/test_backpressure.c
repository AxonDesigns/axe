// Backpressure: thousands of command buffers reach the core while it is not stepping. None may
// be refused or dropped, all must apply in order at the next frame, and events must keep
// flowing to logic afterwards. Runs under both schedulers.
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "runtime/app_internal.h"
#include "test.h"
#include "ui/ui.h"

#define FLOOD 5000
#define WIDTH 200u
#define HEIGHT 100u

typedef struct counting_logic {
    _Atomic int frames_seen; // frame_committed events received
} counting_logic;

static ui_result count_events(void *user, const void *events, size_t size, ui_cmd_writer *cmds) {
    (void)cmds;
    counting_logic *l = user;
    ui_event_reader r;
    ui_result res = ui_event_reader_init(&r, events, size);
    ui_event ev;
    while (res == UI_OK && (res = ui_event_next(&r, &ev)) == UI_OK) {
        if (ev.type == UI_EVENT_FRAME_COMMITTED) atomic_fetch_add(&l->frames_seen, 1);
    }
    return res == UI_ERR_END ? UI_OK : res;
}

typedef struct flood {
    ui_app *app;
    ui_handle surface;
    bool ok;
} flood;

// Buffer i destroys the node of buffer i-1 and recreates its slot with a newer generation,
// then places it at x = i % 100. Out-of-order application would hit stale handles (counted
// as drops); a lost buffer would leave a stale node or the wrong final position.
static int run_flood(void *arg) {
    flood *f = arg;
    ui_handle_allocator *handles;
    ui_cmd_writer *w;
    f->ok = ui_handle_allocator_create(&handles) == UI_OK && ui_cmd_writer_create(&w) == UI_OK;
    ui_handle node = {0};
    for (int i = 0; f->ok && i < FLOOD; i++) {
        if (i) {
            ui_cmd_destroy_node(w, node);
            ui_handle_free(handles, node);
        }
        ui_handle_alloc(handles, &node);
        ui_cmd_create_node(w, node, f->surface);
        ui_cmd_set_rect(w, node, (float)(i % 100), 40, 20, 20);
        ui_cmd_set_color(w, node, 1, 1, 1, 1);
        uint8_t *bytes;
        size_t size;
        ui_cmd_writer_take(w, &bytes, &size);
        ui_msg msg = {.type = UI_MSG_CMD_BUFFER, .ptr = bytes, .size = size};
        // The logic -> core channel: exactly what the logic actor pushes to.
        if (ui_queue_push(&f->app->core.from_logic, &msg) != UI_OK) {
            free(bytes);
            f->ok = false;
        }
    }
    ui_cmd_writer_destroy(w);
    ui_handle_allocator_destroy(handles);
    return 0;
}

static int red_at(const ui_image *img, uint32_t x, uint32_t y) {
    return img->pixels[(size_t)y * img->stride + 4u * x];
}

static void run(ui_scheduler_kind scheduler) {
    counting_logic logic = {0};
    ui_app_desc desc = {
        .scheduler = scheduler,
        .surface = {.kind = UI_SURFACE_HEADLESS, .width = WIDTH, .height = HEIGHT},
        .logic = {.user = &logic, .on_events = count_events},
        .start_paused = true,
    };
    ui_app *app;
    if (ui_app_create(&desc, &app) != UI_OK) {
        fprintf(stderr, "ui_app_create failed\n");
        test_failures++;
        return;
    }
    CHECK(ui_app_frame(app) == UI_OK);
    int seen = atomic_load(&logic.frames_seen);
    CHECK_EQ_INT(seen, 1);

    // Flood while the core is not stepping (nothing calls ui_app_frame meanwhile). Under the
    // threaded scheduler the flood comes from another thread, as logic's would.
    flood f = {.app = app, .surface = ui_app_main_surface(app)};
    if (scheduler == UI_SCHEDULER_THREADED) {
        thrd_t t;
        CHECK(thrd_create(&t, run_flood, &f) == thrd_success);
        thrd_join(t, NULL);
    } else {
        run_flood(&f);
    }
    CHECK(f.ok);
    CHECK_EQ_INT(ui_queue_count(&app->core.from_logic), FLOOD); // nothing refused or lost

    ui_app_stats before, after;
    ui_app_get_stats(app, &before);
    CHECK(ui_app_frame(app) == UI_OK);
    ui_app_get_stats(app, &after);
    // Every op of every buffer applied, none dropped: buffer 0 has 3 ops, the others 4.
    CHECK_EQ_INT(after.commands_applied - before.commands_applied, 3 + 4 * (FLOOD - 1));
    CHECK_EQ_INT(after.commands_dropped, 0);
    CHECK_EQ_INT(after.buffers_rejected, 0);
    CHECK_EQ_INT(after.frames_committed, before.frames_committed + 1); // all in one frame

    // Only the last buffer's node is left, at x = 4999 % 100 = 99.
    ui_image img = {0};
    CHECK(ui_surface_capture(app, ui_app_main_surface(app), &img) == UI_OK);
    if (img.pixels) {
        CHECK(red_at(&img, 105, 50) > 200); // inside x 99..119
        CHECK(red_at(&img, 97, 50) < 60);   // left of it: background
        CHECK(red_at(&img, 50, 50) < 60);   // where earlier buffers put it: gone
        ui_image_free(&img);
    }

    // Event delivery continues: the flood's frame reached logic, and so does the next.
    CHECK_EQ_INT(atomic_load(&logic.frames_seen), seen + 1);
    ui_cmd_writer *w;
    ui_cmd_writer_create(&w);
    ui_cmd_set_color(w, (ui_handle){0, 1}, 0, 0, 0, 0); // stale on purpose: dropped, not lost
    uint8_t *bytes;
    size_t size;
    ui_cmd_writer_take(w, &bytes, &size);
    ui_cmd_writer_destroy(w);
    CHECK(ui_queue_push(&app->core.from_logic,
                        &(ui_msg){.type = UI_MSG_CMD_BUFFER, .ptr = bytes, .size = size}) == UI_OK);
    CHECK(ui_app_frame(app) == UI_OK);
    ui_app_get_stats(app, &after);
    CHECK_EQ_INT(after.commands_dropped, 1);
    ui_app_destroy(app);
}

static void test_flood_single(void) { run(UI_SCHEDULER_SINGLE); }
static void test_flood_threaded(void) { run(UI_SCHEDULER_THREADED); }

int main(void) {
    RUN(test_flood_single);
    RUN(test_flood_threaded);
    return test_failures ? 1 : 0;
}
