// Single-threaded scheduler: core, logic and render run in order on the main thread.
#include "runtime/app_internal.h"

static void step_all(ui_app *app) {
    ui_core_step(&app->core);
    app->logic.actor.step(app->logic.actor.self);
    ui_actor *render = ui_render_actor(app->render);
    render->step(render->self);
}

static ui_result start(ui_app *app) {
    ui_result r = app->logic.actor.start(app->logic.actor.self);
    if (r != UI_OK) return r;
    atomic_store(&app->logic.actor.start_done, true);
    ui_actor *render = ui_render_actor(app->render);
    r = render->start(render->self);
    if (r != UI_OK) {
        app->logic.actor.stop(app->logic.actor.self);
        return r;
    }
    atomic_store(&render->start_done, true);
    return UI_OK;
}

static void stop(ui_app *app) {
    app->logic.actor.stop(app->logic.actor.self);
    ui_actor *render = ui_render_actor(app->render);
    render->stop(render->self);
}

static ui_result run(ui_app *app) {
    while (!app->core.quit_requested) {
        uint64_t timeout = ui_core_has_work(&app->core) ? 0 : UINT64_MAX;
        ui_result r = app->platform->vt->wait(app->platform, timeout);
        if (r != UI_OK) return r;
        step_all(app);
    }
    return app->core.failure;
}

static ui_result frame(ui_app *app) {
    ui_core_force_frame(&app->core);
    step_all(app);
    // Pick up the logic's flush and the render's FRAME_DONE without starting a new frame.
    ui_core_step(&app->core);
    return app->core.failure;
}

static void wait_until(ui_app *app, _Atomic bool *done) {
    ui_actor *render = ui_render_actor(app->render);
    while (!atomic_load(done)) render->step(render->self);
}

const ui_scheduler_ops ui_scheduler_single = {
    .name = "single",
    .start = start,
    .stop = stop,
    .run = run,
    .frame = frame,
    .wait_until = wait_until,
};
