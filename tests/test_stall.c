// A blocked logic actor must not stop the frame loop. Under the threaded scheduler, logic is
// held inside its event callback for 500 ms of (stepped) test time while a spring it started
// is in flight: frames keep committing and the rect follows the spring curve. Once released,
// the commands logic wrote during the stall apply.
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "core/spring.h"
#include "runtime/app_internal.h"
#include "test.h"
#include "ui/ui.h"

#define WIDTH 300u
#define HEIGHT 100u
#define FROM_X 20.0f
#define TO_X 220.0f
#define STIFFNESS 170.0f
#define DAMPING 18.25f
#define STEP_NS 16000000ull // 16 ms frames
#define STALL_FRAMES 32     // 512 ms of test time

typedef struct stall_logic {
    ui_handle_allocator *handles;
    ui_handle node;
    int phase; // 0: wait for the surface, 1: spring sent, 2: stalled and released
    mtx_t lock;
    cnd_t cond;
    bool blocked, release;
} stall_logic;

static ui_result on_events(void *user, const void *events, size_t size, ui_cmd_writer *cmds) {
    stall_logic *l = user;
    ui_event_reader r;
    ui_result res = ui_event_reader_init(&r, events, size);
    ui_event ev;
    while (res == UI_OK && (res = ui_event_next(&r, &ev)) == UI_OK) {
        if (ev.type == UI_EVENT_SURFACE_RESIZED && l->phase == 0) {
            ui_handle_alloc(l->handles, &l->node);
            ui_cmd_create_node(cmds, l->node, ev.surface);
            ui_cmd_set_rect(cmds, l->node, FROM_X, 30, 40, 40);
            ui_cmd_set_color(cmds, l->node, 1, 1, 1, 1);
            float target[4] = {TO_X, 30, 40, 40};
            ui_cmd_start_spring(cmds, l->node, UI_PROP_RECT, target, STIFFNESS, DAMPING);
            l->phase = 1;
        } else if (ev.type == UI_EVENT_FRAME_COMMITTED && l->phase == 1 &&
                   ev.frame_committed.frame_id >= 2) {
            // The spring is running in the core. Block like a long logic turn, until the test
            // says so; then write commands that can only apply after the stall.
            mtx_lock(&l->lock);
            l->blocked = true;
            cnd_broadcast(&l->cond);
            while (!l->release) cnd_wait(&l->cond, &l->lock);
            mtx_unlock(&l->lock);
            ui_cmd_set_color(cmds, l->node, 1, 0, 0, 1);
            l->phase = 2;
        }
    }
    return res == UI_ERR_END ? UI_OK : res;
}

static int left_edge(const ui_image *img, uint32_t y) {
    for (uint32_t x = 0; x < img->width; x++) {
        if (img->pixels[(size_t)y * img->stride + 4u * x + 1] > 128) return (int)x; // >50%
    }
    return -1;
}

static void test_frames_continue_while_logic_blocked(void) {
    stall_logic logic = {0};
    ui_handle_allocator_create(&logic.handles);
    mtx_init(&logic.lock, mtx_plain);
    cnd_init(&logic.cond);
    ui_app_desc desc = {
        .scheduler = UI_SCHEDULER_THREADED,
        .surface = {.kind = UI_SURFACE_HEADLESS, .width = WIDTH, .height = HEIGHT},
        .logic = {.user = &logic, .on_events = on_events},
        .start_paused = true,
    };
    ui_app *app;
    if (ui_app_create(&desc, &app) != UI_OK) {
        fprintf(stderr, "ui_app_create failed\n");
        test_failures++;
        return;
    }
    CHECK(ui_app_frame(app) == UI_OK); // logic creates the node and starts the spring
    // Frame 2 applies them at t=0; its frame_committed event blocks logic. Waiting for logic
    // now would hang, so from here frames only wait for render.
    CHECK(ui_app_frame_without_logic(app) == UI_OK);
    mtx_lock(&logic.lock);
    while (!logic.blocked) cnd_wait(&logic.cond, &logic.lock);
    mtx_unlock(&logic.lock);

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
        if (i <= 8) CHECK(left > previous); // clearly moving while logic is blocked
        previous = left;
        ui_image_free(&img);
    }
    CHECK(logic.phase == 1); // logic really was blocked the whole time

    // Release logic: the set_color it wrote during the stall must apply afterwards.
    mtx_lock(&logic.lock);
    logic.release = true;
    cnd_broadcast(&logic.cond);
    mtx_unlock(&logic.lock);
    CHECK(ui_app_frame(app) == UI_OK); // waits for logic, whose commands are now queued
    CHECK(ui_app_frame(app) == UI_OK); // applies them
    CHECK(logic.phase == 2);
    // The rect has nearly settled at TO_X; its center must now be red.
    ui_image img = {0};
    CHECK(ui_surface_capture(app, ui_app_main_surface(app), &img) == UI_OK);
    if (img.pixels) {
        const uint8_t *p = img.pixels + 50u * img.stride + 4u * (uint32_t)(TO_X + 20);
        CHECK(p[0] > 200 && p[1] < 60 && p[2] < 60);
        ui_image_free(&img);
    }
    ui_app_get_stats(app, &stats);
    CHECK_EQ_INT(stats.commands_dropped, 0);
    ui_app_destroy(app);
    cnd_destroy(&logic.cond);
    mtx_destroy(&logic.lock);
    ui_handle_allocator_destroy(logic.handles);
}

int main(void) {
    RUN(test_frames_continue_while_logic_blocked);
    return test_failures ? 1 : 0;
}
