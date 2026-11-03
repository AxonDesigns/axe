// The app object and the scheduler interface.
#pragma once

#include "core/core.h"
#include "hooks/hooks_internal.h"
#include "platform/platform.h"
#include "render/render.h"
#include "runtime/jobs.h"
#include "runtime/logic_actor.h"
#include "ui/app.h"

typedef struct ui_scheduler_ops {
    const char *name;
    // Runs every actor's start() (on the thread that will run it) and waits for them.
    ui_result (*start)(ui_app *app);
    // Stops every actor (stop() on its own thread) and joins threads.
    void (*stop)(ui_app *app);
    // ui_app_run: frames paced by the platform until quit.
    ui_result (*run)(ui_app *app);
    // ui_app_frame: one frame, then wait until idle.
    ui_result (*frame)(ui_app *app);
    // ui_app_frame_without_logic: one frame, waiting for render only.
    ui_result (*frame_without_logic)(ui_app *app);
    // Lets the actors process messages just queued by the main thread, then returns when
    // `done` is true.
    void (*wait_until)(ui_app *app, _Atomic bool *done);
    // Takes the command buffers queued so far and waits until they are prepared, so the
    // next frame applies them (ui_app_frame and ui_app_step_core are deterministic).
    void (*wait_prepared)(ui_app *app);
} ui_scheduler_ops;

extern const ui_scheduler_ops ui_scheduler_threaded;
extern const ui_scheduler_ops ui_scheduler_single;

// Test-only: like ui_app_frame, but returns once the render actor has finished the frame,
// without waiting for logic to handle its events. Lets a test keep producing frames while
// the logic actor is blocked. Thread: main.
ui_result ui_app_frame_without_logic(ui_app *app);

// Test-only: one frame produced by the core alone, on the calling (main) thread. The display
// list is published, and its atlas update queued, but the render actor is not stepped or
// waited for, so the next one of these replaces an undrawn list. Valid under both
// schedulers while ui_app_run/ui_app_frame is not executing. Thread: main.
ui_result ui_app_step_core(ui_app *app);

struct ui_app {
    ui_logger log;
    ui_hooks hooks;
    const ui_scheduler_ops *sched;
    ui_platform *platform;
    ui_signal progress;  // notified on messages to the core and on capture completion
    ui_jobs *jobs;       // worker threads for any module (created before the core)
    ui_core core;
    ui_logic logic;
    ui_render *render;
    ui_handle main_surface;
    bool started;
    bool running;        // inside ui_app_run or ui_app_frame
    ui_trace_printer *printer;
};
