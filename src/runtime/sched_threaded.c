// Threaded scheduler: the core runs on the main thread, logic and render on their own.
#include <stdatomic.h>
#include <stdlib.h>
#include <threads.h>

#include "runtime/app_internal.h"

#define MODULE "sched"

typedef struct thread_ctx {
    ui_actor *actor;
    ui_signal *progress;
} thread_ctx;

static int actor_thread(void *arg) {
    thread_ctx ctx = *(thread_ctx *)arg;
    free(arg);
    ui_actor *a = ctx.actor;
    ui_signal *progress = ctx.progress;
    a->start_result = a->start(a->self);
    atomic_store(&a->start_done, true);
    ui_signal_notify(progress);
    if (a->start_result != UI_OK) return 0;
    while (!atomic_load(&a->stopping)) {
        // Read the sequence before stepping so a push during step() is never missed.
        uint64_t seen = ui_signal_seq(&a->signal);
        a->step(a->self);
        if (atomic_load(&a->stopping)) break;
        ui_signal_wait(&a->signal, seen, 0);
    }
    // Drain what was sent before the stop (e.g. the close event for logic), as the single
    // scheduler does by stepping every actor once more before its loop ends.
    a->step(a->self);
    a->stop(a->self);
    return 0;
}

static ui_result spawn(ui_app *app, ui_actor *a) {
    thread_ctx *ctx = malloc(sizeof *ctx);
    if (!ctx) return UI_ERR_OUT_OF_MEMORY;
    *ctx = (thread_ctx){a, &app->progress};
    if (thrd_create(&a->thread, actor_thread, ctx) != thrd_success) {
        free(ctx);
        return UI_ERR_OUT_OF_MEMORY;
    }
    a->has_thread = true;
    return UI_OK;
}

static void join(ui_actor *a) {
    if (!a->has_thread) return;
    atomic_store(&a->stopping, true);
    ui_signal_notify(&a->signal);
    thrd_join(a->thread, NULL);
    a->has_thread = false;
}

static ui_result wait_started(ui_app *app, ui_actor *a) {
    for (;;) {
        uint64_t seen = ui_signal_seq(&app->progress);
        if (atomic_load(&a->start_done)) return a->start_result;
        ui_signal_wait(&app->progress, seen, 0);
    }
}

static void stop(ui_app *app) {
    join(&app->logic.actor);
    join(ui_render_actor(app->render));
}

static ui_result start(ui_app *app) {
    ui_actor *render = ui_render_actor(app->render);
    ui_result r = spawn(app, render);
    if (r == UI_OK) r = wait_started(app, render);
    if (r == UI_OK) r = spawn(app, &app->logic.actor);
    if (r == UI_OK) r = wait_started(app, &app->logic.actor);
    if (r != UI_OK) {
        // A thread whose start failed has already returned; join() just reaps it.
        stop(app);
    }
    return r;
}

static ui_result run(ui_app *app) {
    while (!app->core.quit_requested) {
        uint64_t timeout = ui_core_has_work(&app->core) ? 0 : UINT64_MAX;
        ui_result r = app->platform->vt->wait(app->platform, timeout);
        if (r != UI_OK) return r;
        ui_core_step(&app->core);
    }
    return app->core.failure;
}

static ui_result frame(ui_app *app) {
    ui_core_force_frame(&app->core);
    ui_core_step(&app->core);
    // Wait for render to finish the frame and logic to handle its events. Every message to
    // the core notifies `progress`, so this wakes exactly when there is something to check.
    for (;;) {
        uint64_t seen = ui_signal_seq(&app->progress);
        ui_core_step(&app->core); // drains; never starts a frame (nothing is ready)
        if (ui_core_idle(&app->core) || app->core.quit_requested) break;
        ui_signal_wait(&app->progress, seen, 0);
    }
    return app->core.failure;
}

static void wait_until(ui_app *app, _Atomic bool *done) {
    for (;;) {
        uint64_t seen = ui_signal_seq(&app->progress);
        if (atomic_load(done)) return;
        ui_signal_wait(&app->progress, seen, 0);
    }
}

const ui_scheduler_ops ui_scheduler_threaded = {
    .name = "threaded",
    .start = start,
    .stop = stop,
    .run = run,
    .frame = frame,
    .wait_until = wait_until,
};
