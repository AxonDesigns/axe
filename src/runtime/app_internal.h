// The app object and the scheduler interface.
#pragma once

#include "core/core.h"
#include "hooks/hooks_internal.h"
#include "platform/platform.h"
#include "render/render.h"
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
    // Lets the actors process messages just queued by the main thread, then returns when
    // `done` is true.
    void (*wait_until)(ui_app *app, _Atomic bool *done);
} ui_scheduler_ops;

extern const ui_scheduler_ops ui_scheduler_threaded;
extern const ui_scheduler_ops ui_scheduler_single;

struct ui_app {
    ui_logger log;
    ui_hooks hooks;
    const ui_scheduler_ops *sched;
    ui_platform *platform;
    ui_signal progress;  // notified on messages to the core and on capture completion
    ui_core core;
    ui_logic logic;
    ui_render *render;
    ui_handle main_surface;
    bool started;
    bool running;        // inside ui_app_run or ui_app_frame
    ui_trace_printer *printer;
};
