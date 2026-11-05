// axe-run: runs a script app bundle (docs/typescript.md, "Running apps").
//
//   axe-run [options] bundle.{js,axbc} [-- app arguments]
//
// Options:
//   --scheduler single|threaded   as UI_SCHEDULER (default threaded)
//   --headless                    offscreen with a paused clock: frames up to --at, then a PPM
//   --at S                        headless: steps the clock at 60 Hz to S seconds (default 0)
//   --out FILE                    headless: the image to write (default out.ppm)
//   --scale N                     headless: the scale in 120ths (default 120)
//   --size WxH                    the surface's logical size (default 800x600)
//   --title TEXT                  the window's title
//   --font FILE                   registers a font at startup (repeatable)
//   --no-system-fonts             only registered fonts
//   --memory-limit MB             the JS heap's limit (default none)
//   --long-task MS                report tasks longer than this (default 50)
//   --strict                      exit 3 if the app reported an uncaught error
//
// Built by axe_ts_app (cmake/TypeScript.cmake), the same program embeds an app: its bundle
// (AXE_RUN_EMBED, bytecode, in release builds) or the path of its source (AXE_RUN_BUNDLE, in
// debug builds), and its defaults (AXE_RUN_WIDTH, AXE_RUN_HEIGHT, AXE_RUN_SCALE, AXE_RUN_TITLE,
// AXE_RUN_FONT). A bundle on the command line replaces it.
//
// Environment: UI_SCHEDULER, UI_GPU=software, UI_LOG, UI_TRACE.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AXE_DEV
#include "runtime/app_internal.h"
#include "script/host/host.h"
#include "session.h"
#endif
#include "ui/ui.h"

#ifndef AXE_RUN_WIDTH
#define AXE_RUN_WIDTH 800
#endif
#ifndef AXE_RUN_HEIGHT
#define AXE_RUN_HEIGHT 600
#endif
#ifndef AXE_RUN_SCALE
#define AXE_RUN_SCALE 120
#endif
#ifndef AXE_RUN_TITLE
#define AXE_RUN_TITLE "Axe"
#endif

#ifdef AXE_RUN_EMBED
static const unsigned char embedded[] = {
#embed AXE_RUN_EMBED
};
#endif

#define MAX_FONTS 16

static atomic_uint errors;

static void on_trace(void *user, const ui_trace_event *ev) {
    (void)user;
    if (ev->kind == UI_TRACE_SCRIPT_EXCEPTION || ev->kind == UI_TRACE_SCRIPT_REJECTION) atomic_fetch_add(&errors, 1);
}

static int usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--scheduler single|threaded] [--headless [--at S] [--out f.ppm] [--scale N]]\n"
            "       [--size WxH] [--title T] [--font F]... [--no-system-fonts] [--memory-limit MB]\n"
            "       [--long-task MS] [--strict] [--dev [--dev-root DIR --dev-entry FILE]] %s\n"
            "       [-- app arguments]\n",
            argv0,
#if defined(AXE_RUN_EMBED) || defined(AXE_RUN_BUNDLE)
            "[bundle.{js,axbc}]"
#else
            "bundle.{js,axbc}"
#endif
    );
    return 2;
}

#ifdef AXE_DEV
static void wake_core(void *user) { ui_app_request_turn(user); }
#endif

int main(int argc, char **argv) {
    bool headless = false, strict = false, system_fonts = true, dev = false;
    double at = 0, memory_mb = 0, long_task_ms = 0;
    uint32_t width = AXE_RUN_WIDTH, height = AXE_RUN_HEIGHT, scale = AXE_RUN_SCALE;
    const char *out = "out.ppm", *title = AXE_RUN_TITLE, *bundle = NULL;
    const char *fonts[MAX_FONTS];
    uint32_t font_count = 0;
    ui_scheduler_kind scheduler = UI_SCHEDULER_FROM_ENV;
#ifdef AXE_RUN_FONT
    fonts[font_count++] = AXE_RUN_FONT;
#endif
#ifdef AXE_RUN_DEV_ROOT
    const char *dev_root = AXE_RUN_DEV_ROOT, *dev_entry = AXE_RUN_DEV_ENTRY, *dev_work = AXE_RUN_DEV_WORK;
#elif defined(AXE_DEV)
    const char *dev_root = NULL, *dev_entry = NULL, *dev_work = "axe-dev";
#endif
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        bool more = i + 1 < argc;
        if (!strcmp(a, "--")) {
            i++;
            break;
        } else if (!strcmp(a, "--headless")) headless = true;
        else if (!strcmp(a, "--strict")) strict = true;
        else if (!strcmp(a, "--no-system-fonts")) system_fonts = false;
        else if (!strcmp(a, "--scheduler") && more) {
            const char *s = argv[++i];
            if (!strcmp(s, "single")) scheduler = UI_SCHEDULER_SINGLE;
            else if (!strcmp(s, "threaded")) scheduler = UI_SCHEDULER_THREADED;
            else return usage(argv[0]);
        } else if (!strcmp(a, "--at") && more) at = atof(argv[++i]);
        else if (!strcmp(a, "--out") && more) out = argv[++i];
        else if (!strcmp(a, "--scale") && more) scale = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(a, "--size") && more) {
            if (sscanf(argv[++i], "%ux%u", &width, &height) != 2) return usage(argv[0]);
        } else if (!strcmp(a, "--title") && more) title = argv[++i];
        else if (!strcmp(a, "--font") && more) {
            if (font_count == MAX_FONTS) return usage(argv[0]);
            fonts[font_count++] = argv[++i];
        } else if (!strcmp(a, "--memory-limit") && more) memory_mb = atof(argv[++i]);
        else if (!strcmp(a, "--long-task") && more) long_task_ms = atof(argv[++i]);
        else if (!strcmp(a, "--dev")) dev = true;
#ifdef AXE_DEV
        else if (!strcmp(a, "--dev-root") && more) dev_root = argv[++i];
        else if (!strcmp(a, "--dev-entry") && more) dev_entry = argv[++i];
#endif
        else if (a[0] != '-' && !bundle) bundle = a;
        else return usage(argv[0]);
    }

#ifndef AXE_DEV
    if (dev) {
        fprintf(stderr, "--dev is only in development builds: Debug, or the AXE_DEV CMake option (docs/ui.md, \"Hot reload\")\n");
        return 2;
    }
#endif

    ui_script_desc script = {
        .bundle_path = bundle,
        .args = (const char *const *)(argv + i),
        .arg_count = (uint32_t)(argc - i),
        .memory_limit = (size_t)(memory_mb * 1024 * 1024),
        .long_task_ns = (uint64_t)(long_task_ms * 1e6),
    };
#ifdef AXE_DEV
    ui_dev_channel *channel = NULL;
    ui_script_internal internal = {0};
    if (dev) {
        if (!dev_root || !dev_entry) return usage(argv[0]);
        if (ui_dev_channel_create(&(ui_dev_desc){.entry = dev_entry, .packages = AXE_DEV_PACKAGES}, &channel) != UI_OK)
            return 1;
        internal.dev = channel;
        script.internal = &internal;
        script.bundle_path = NULL;
    }
#endif
    if (!bundle && !dev) {
#if defined(AXE_RUN_EMBED)
        script.bundle = embedded;
        script.bundle_size = sizeof embedded;
        script.bundle_name = AXE_RUN_NAME;
#elif defined(AXE_RUN_BUNDLE)
        script.bundle_path = AXE_RUN_BUNDLE;
#else
        return usage(argv[0]);
#endif
    }
    ui_app_desc desc = {
        .scheduler = scheduler,
        .surface = {.kind = headless ? UI_SURFACE_HEADLESS : UI_SURFACE_WINDOW, .width = width, .height = height,
                    .scale_120 = headless ? scale : 0, .title = title},
        .script = &script,
        .start_paused = headless,
        .text = {.font_files = fonts, .font_file_count = font_count, .system_fonts = system_fonts},
    };
    ui_app *app;
    ui_result r = ui_app_create(&desc, &app);
    if (r != UI_OK) {
        fprintf(stderr, "cannot start: %s\n", ui_result_str(r));
        return 1;
    }
    ui_hooks_subscribe(app, on_trace, NULL, NULL);
#ifdef AXE_DEV
    ui_dev_session *session = NULL;
    ui_dev_build_desc build = {dev_root, dev_work, AXE_TSX_PATH, AXE_ESBUILD_PATH, AXE_DEV_SOURCE_DIR};
    if (dev) {
        ui_dev_channel_set_wake(channel, wake_core, app);
        session = ui_dev_session_start(&build, channel);
        if (!session) {
            fprintf(stderr, "cannot watch %s\n", dev_root);
            ui_app_destroy(app);
            ui_dev_channel_destroy(channel);
            return 1;
        }
    }
#endif
    int status = 0;
    if (headless) {
        // As the C examples step it: two frames build the scene, then 60 Hz steps of the paused
        // clock up to --at, then the frame that is captured.
        r = ui_app_frame(app);
        if (r == UI_OK) r = ui_app_frame(app);
        const uint64_t frame_ns = 16666667;
        for (uint64_t t = 0; r == UI_OK && (double)t < at * 1e9; t += frame_ns) {
            ui_app_clock_step(app, frame_ns);
            r = ui_app_frame(app);
        }
        ui_image image;
        if (r == UI_OK) r = ui_app_frame(app);
        if (r == UI_OK) r = ui_surface_capture(app, ui_app_main_surface(app), &image);
        if (r == UI_OK) r = ui_image_write_ppm(&image, out);
        if (r == UI_OK) {
            printf("wrote %s (%ux%u)\n", out, image.width, image.height);
            ui_image_free(&image);
        } else {
            fprintf(stderr, "headless frame failed: %s\n", ui_result_str(r));
            status = 1;
        }
    } else {
        r = ui_app_run(app);
        if (r != UI_OK) {
            fprintf(stderr, "run failed: %s\n", ui_result_str(r));
            status = 1;
        }
    }
#ifdef AXE_DEV
    ui_dev_session_stop(session); // before the app it wakes
#endif
    ui_app_destroy(app);
#ifdef AXE_DEV
    ui_dev_channel_destroy(channel);
#endif
    if (strict && atomic_load(&errors) && !status) {
        fprintf(stderr, "%u uncaught error(s) reported\n", atomic_load(&errors));
        status = 3;
    }
    return status;
}
