// A busy JS loop must not stop the frame loop (docs/script.md, "The turn"). The app
// (tests/ts/stall.ts) starts a spring, then spins in JS from its second frame until released:
// frames keep committing and the rect follows the spring curve meanwhile. Once released, what
// JS wrote afterwards applies. As test_stall.c does for C logic.
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "core/spring.h"
#include "runtime/app_internal.h"
#include "script/host/host.h"
#include "test.h"
#include "ui/ui.h"

#define WIDTH 300u
#define HEIGHT 100u
#define FROM_X 20.0f
#define TO_X 220.0f
#define STIFFNESS 170.0f
#define DAMPING 18.25f
#define STEP_NS 16000000ull
#define STALL_FRAMES 32

typedef struct stall {
    mtx_t lock;
    cnd_t cond;
    bool blocked;
    atomic_bool release;
} stall;

static ui_js_val js_blocked(ui_js *js, const ui_js_call_info *call) {
    stall *s = call->data;
    mtx_lock(&s->lock);
    s->blocked = true;
    cnd_broadcast(&s->cond);
    mtx_unlock(&s->lock);
    return ui_js_undefined(js);
}

static ui_js_val js_released(ui_js *js, const ui_js_call_info *call) {
    stall *s = call->data;
    return ui_js_bool(js, atomic_load(&s->release));
}

static void setup(void *user, ui_js *js) {
    ui_js_val m = ui_js_object(js);
    ui_js_set(js, m, "blocked", ui_js_function(js, "blocked", 0, js_blocked, user));
    ui_js_set(js, m, "released", ui_js_function(js, "released", 0, js_released, user));
    ui_js_define_module(js, "axe:stall", m);
}

static int left_edge(const ui_image *img, uint32_t y) {
    for (uint32_t x = 0; x < img->width; x++) {
        if (img->pixels[(size_t)y * img->stride + 4u * x + 1] > 128) return (int)x;
    }
    return -1;
}

static void test_frames_continue_while_js_spins(void) {
    stall s = {0};
    mtx_init(&s.lock, mtx_plain);
    cnd_init(&s.cond);
    ui_script_internal internal = {.setup = setup, .user = &s};
    ui_script_desc script = {.bundle_path = UI_STALL_BUNDLE, .internal = &internal, .long_task_ns = 1000000000000ull};
    ui_app_desc desc = {
        .scheduler = UI_SCHEDULER_THREADED,
        .surface = {.kind = UI_SURFACE_HEADLESS, .width = WIDTH, .height = HEIGHT},
        .script = &script,
        .start_paused = true,
    };
    ui_app *app;
    if (ui_app_create(&desc, &app) != UI_OK) {
        test_failures++;
        return;
    }
    CHECK(ui_app_frame(app) == UI_OK); // JS creates the node and starts the spring
    CHECK(ui_app_frame_without_logic(app) == UI_OK); // applies them; its frame event stalls JS
    mtx_lock(&s.lock);
    while (!s.blocked) cnd_wait(&s.cond, &s.lock);
    mtx_unlock(&s.lock);

    ui_app_stats stats;
    ui_app_get_stats(app, &stats);
    uint64_t committed = stats.frames_committed;
    int previous = (int)FROM_X;
    for (int i = 1; i <= STALL_FRAMES; i++) {
        ui_app_clock_step(app, STEP_NS);
        CHECK(ui_app_frame_without_logic(app) == UI_OK);
        ui_app_get_stats(app, &stats);
        CHECK_EQ_INT(stats.frames_committed, committed + (uint64_t)i); // one commit per frame
        ui_image img = {0};
        CHECK(ui_surface_capture(app, ui_app_main_surface(app), &img) == UI_OK);
        if (!img.pixels) break;
        double x, v;
        ui_spring_solve(STIFFNESS, DAMPING, FROM_X - TO_X, 0.0, (double)(i * STEP_NS) * 1e-9, &x, &v);
        int left = left_edge(&img, 50);
        CHECK_NEAR(left, TO_X + x, 1.5);
        if (i <= 8) CHECK(left > previous); // clearly moving while JS spins
        previous = left;
        ui_image_free(&img);
    }

    atomic_store(&s.release, true);
    CHECK(ui_app_frame(app) == UI_OK); // waits for the turn to end; its commands are queued
    CHECK(ui_app_frame(app) == UI_OK); // applies them
    ui_image img = {0};
    CHECK(ui_surface_capture(app, ui_app_main_surface(app), &img) == UI_OK);
    if (img.pixels) {
        const uint8_t *p = img.pixels + 50u * img.stride + 4u * (uint32_t)(TO_X + 20);
        CHECK(p[0] > 200 && p[1] < 60 && p[2] < 60); // the red JS wrote after the stall
        ui_image_free(&img);
    }
    ui_app_get_stats(app, &stats);
    CHECK_EQ_INT(stats.commands_dropped, 0);
    ui_app_destroy(app);
    cnd_destroy(&s.cond);
    mtx_destroy(&s.lock);
}

int main(void) {
    RUN(test_frames_continue_while_js_spins);
    return test_failures ? 1 : 0;
}
