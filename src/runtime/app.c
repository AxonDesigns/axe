#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/app_internal.h"

#define MODULE "app"

static const ui_scheduler_ops *scheduler_from(ui_scheduler_kind kind, const ui_logger *log) {
    if (kind == UI_SCHEDULER_FROM_ENV) {
        const char *env = getenv("UI_SCHEDULER");
        kind = UI_SCHEDULER_THREADED;
        if (env && strcmp(env, "single") == 0) kind = UI_SCHEDULER_SINGLE;
        else if (env && *env && strcmp(env, "threaded") != 0)
            UI_WARN(log, MODULE, "unknown UI_SCHEDULER=%s; using threaded", env);
    }
    return kind == UI_SCHEDULER_SINGLE ? &ui_scheduler_single : &ui_scheduler_threaded;
}

static ui_gpu_preference gpu_from(ui_gpu_preference gpu) {
    if (gpu != UI_GPU_FROM_ENV) return gpu;
    const char *env = getenv("UI_GPU");
    return env && strcmp(env, "software") == 0 ? UI_GPU_SOFTWARE : UI_GPU_HARDWARE;
}

ui_result ui_app_create(const ui_app_desc *desc, ui_app **out) {
    if (!desc || !out) return UI_ERR_INVALID_ARGUMENT;
    ui_app *app = calloc(1, sizeof *app);
    if (!app) return UI_ERR_OUT_OF_MEMORY;
    ui_logger_init_from_env(&app->log);
    ui_hooks_init(&app->hooks);
    app->sched = scheduler_from(desc->scheduler, &app->log);
    ui_result r = ui_signal_init(&app->progress);
    if (r != UI_OK) {
        free(app);
        return r;
    }

    // The single scheduler keeps everything on the main thread: jobs run inline.
    ui_jobs_desc jobs_desc = {
        .log = &app->log,
        .threads = app->sched == &ui_scheduler_single ? 0 : ui_jobs_default_threads(desc->worker_threads),
    };
    r = ui_jobs_create(&jobs_desc, &app->jobs);
    if (r != UI_OK) goto fail_jobs;

    // The core and its listener must exist before the platform, which calls back into it.
    ui_core_desc core_desc = {
        .log = &app->log,
        .hooks = &app->hooks,
        .platform = NULL,
        .time = desc->time,
        .start_paused = desc->start_paused,
        .progress = &app->progress,
        .text = &desc->text,
        .jobs = app->jobs,
    };
    r = ui_core_init(&app->core, &core_desc);
    if (r != UI_OK) goto fail_core;
    ui_platform_listener listener = ui_core_platform_listener(&app->core);
    r = desc->surface.kind == UI_SURFACE_HEADLESS
            ? ui_platform_headless_create(&app->log, listener, &app->platform)
            : ui_platform_wayland_create(&app->log, listener, &app->platform);
    if (r != UI_OK) goto fail_platform;
    app->core.platform = app->platform;

    r = ui_logic_init(&app->logic, &desc->logic, &app->log, &app->core.from_logic);
    if (r != UI_OK) goto fail_logic;
    ui_render_desc render_desc = {
        .log = &app->log,
        .hooks = &app->hooks,
        .gpu = gpu_from(desc->gpu),
        .core_queue = &app->core.actor.queue,
        .progress = &app->progress,
    };
    r = ui_render_create(&render_desc, &app->render);
    if (r != UI_OK) goto fail_render;
    app->core.logic = &app->logic.actor;
    app->core.render = ui_render_actor(app->render);

    r = ui_core_add_surface(&app->core, &desc->surface, &app->main_surface);
    if (r != UI_OK) {
        UI_ERROR(&app->log, MODULE, "cannot create surface: %s", ui_result_str(r));
        goto fail_surface;
    }

    if (ui_env_flag("UI_TRACE")) {
        r = ui_trace_printer_install(app, &app->printer);
        if (r != UI_OK) UI_WARN(&app->log, MODULE, "UI_TRACE ignored: %s", ui_result_str(r));
    }

    r = app->sched->start(app);
    if (r != UI_OK) {
        UI_ERROR(&app->log, MODULE, "cannot start actors: %s", ui_result_str(r));
        goto fail_start;
    }
    app->started = true;
    UI_INFO(&app->log, MODULE, "scheduler: %s", app->sched->name);
    *out = app;
    return UI_OK;

fail_start:
    ui_trace_printer_destroy(app, app->printer);
    ui_core_remove_surfaces(&app->core);
fail_surface:
    ui_render_destroy(app->render);
fail_render:
    ui_logic_destroy(&app->logic);
fail_logic:
    app->platform->vt->destroy(app->platform);
fail_platform:
    ui_jobs_destroy(app->jobs); // before the core whose buffers its jobs work on
    app->jobs = NULL;
    ui_core_destroy(&app->core);
fail_core:
    ui_jobs_destroy(app->jobs);
fail_jobs:
    ui_signal_destroy(&app->progress);
    free(app);
    return r;
}

void ui_app_destroy(ui_app *app) {
    if (!app) return;
    // Render first: GPU surfaces must go before the platform surfaces they draw into.
    if (app->started) app->sched->stop(app);
    ui_trace_printer_destroy(app, app->printer);
    ui_core_remove_surfaces(&app->core);
    ui_render_destroy(app->render);
    ui_logic_destroy(&app->logic);
    ui_jobs_destroy(app->jobs); // before the core whose buffers its jobs work on
    ui_core_destroy(&app->core);
    app->platform->vt->destroy(app->platform);
    ui_signal_destroy(&app->progress);
    free(app);
}

ui_result ui_app_run(ui_app *app) {
    if (app->running) return UI_ERR_WRONG_STATE;
    bool any_window = false;
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        const ui_core_surface *s = &app->core.surfaces[i];
        if (!ui_handle_is_null(s->handle) && s->kind == UI_SURFACE_WINDOW) any_window = true;
    }
    if (!any_window) return UI_ERR_UNSUPPORTED;
    app->running = true;
    app->core.quit_requested = false;
    ui_result r = app->sched->run(app);
    app->running = false;
    return r;
}

void ui_app_quit(ui_app *app) { app->core.quit_requested = true; }

ui_result ui_app_frame(ui_app *app) {
    if (app->running) return UI_ERR_WRONG_STATE;
    app->running = true;
    ui_result r = app->sched->frame(app);
    app->running = false;
    return r;
}

ui_result ui_app_frame_without_logic(ui_app *app) {
    if (app->running) return UI_ERR_WRONG_STATE;
    app->running = true;
    ui_result r = app->sched->frame_without_logic(app);
    app->running = false;
    return r;
}

void ui_app_clock_pause(ui_app *app, bool paused) { ui_clock_pause(&app->core.clock, paused); }

void ui_app_clock_step(ui_app *app, uint64_t delta_ns) {
    ui_clock_step(&app->core.clock, delta_ns);
}

ui_result ui_app_clock_set_scale(ui_app *app, double scale) {
    if (!(scale > 0.0)) return UI_ERR_INVALID_ARGUMENT;
    ui_clock_set_scale(&app->core.clock, scale);
    return UI_OK;
}

uint64_t ui_app_clock_now(const ui_app *app) {
    return ui_clock_at(&app->core.clock, ui_clock_host_now(&app->core.clock));
}

ui_handle ui_app_main_surface(const ui_app *app) { return app->main_surface; }

ui_result ui_surface_set_position(ui_app *app, ui_handle surface, int32_t x, int32_t y) {
    ui_core_surface *s = ui_core_surface_get(&app->core, surface);
    if (!s) return UI_ERR_STALE_HANDLE;
    return app->platform->vt->surface_set_position(s->platform, x, y);
}

ui_result ui_surface_capture(ui_app *app, ui_handle surface, ui_image *out) {
    ui_core_surface *s = ui_core_surface_get(&app->core, surface);
    if (!s) return UI_ERR_STALE_HANDLE;
    if (s->kind != UI_SURFACE_HEADLESS) return UI_ERR_UNSUPPORTED;
    ui_capture_request req = {.surface = surface, .image = out, .result = UI_OK};
    atomic_init(&req.done, false);
    ui_msg msg = {.type = UI_MSG_CAPTURE, .surface = surface, .ptr = &req};
    ui_result r = ui_queue_push(&ui_render_actor(app->render)->queue, &msg);
    if (r != UI_OK) return r;
    app->sched->wait_until(app, &req.done);
    return req.result;
}

void ui_image_free(ui_image *image) {
    free(image->pixels);
    image->pixels = NULL;
}

ui_result ui_image_write_ppm(const ui_image *image, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return UI_ERR_IO;
    fprintf(f, "P6\n%u %u\n255\n", image->width, image->height);
    bool ok = true;
    for (uint32_t y = 0; y < image->height && ok; y++) {
        const uint8_t *row = image->pixels + (size_t)y * image->stride;
        for (uint32_t x = 0; x < image->width && ok; x++) ok = fwrite(row + 4 * x, 1, 3, f) == 3;
    }
    if (fclose(f) != 0) ok = false;
    return ok ? UI_OK : UI_ERR_IO;
}

void ui_app_get_stats(const ui_app *app, ui_app_stats *out) { *out = app->core.stats; }

ui_result ui_app_step_core(ui_app *app) {
    if (app->running) return UI_ERR_WRONG_STATE;
    app->sched->wait_prepared(app);
    ui_core_force_frame(&app->core);
    ui_core_step(&app->core);
    return app->core.failure;
}
