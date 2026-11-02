// spring_rect: one rounded rect driven by springs from the logic actor.
//
//   spring_rect                      window on the Wayland compositor
//   spring_rect --headless [--seconds S] [--out file.ppm]
//                                    offscreen, paused clock stepped at 60 Hz, writes a PPM
//
// Environment: UI_SCHEDULER=threaded|single, UI_GPU=software, UI_TRACE=1, UI_LOG=debug,
// UI_STALL_LOGIC=1.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "demo_logic.h"
#include "ui/ui.h"

static int run_headless(ui_app *app, double seconds, const char *out) {
    uint64_t frames = (uint64_t)(seconds * 60.0);
    for (uint64_t i = 0; i <= frames; i++) {
        if (i) ui_app_clock_step(app, 1000000000ull / 60);
        ui_result r = ui_app_frame(app);
        if (r != UI_OK) {
            fprintf(stderr, "frame failed: %s\n", ui_result_str(r));
            return 1;
        }
    }
    ui_image image;
    ui_result r = ui_surface_capture(app, ui_app_main_surface(app), &image);
    if (r == UI_OK) r = ui_image_write_ppm(&image, out);
    if (r == UI_OK) {
        printf("wrote %s (%ux%u)\n", out, image.width, image.height);
        ui_image_free(&image);
    }
    if (r != UI_OK) fprintf(stderr, "capture failed: %s\n", ui_result_str(r));
    return r == UI_OK ? 0 : 1;
}

int main(int argc, char **argv) {
    bool headless = false;
    double seconds = 3.0;
    const char *out = "spring_rect.ppm";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0) headless = true;
        else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) seconds = atof(argv[++i]);
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) out = argv[++i];
        else {
            fprintf(stderr, "usage: %s [--headless [--seconds S] [--out file.ppm]]\n", argv[0]);
            return 2;
        }
    }

    demo_logic logic;
    if (demo_logic_init(&logic) != UI_OK) return 1;
    ui_app_desc desc = {
        .surface = {
            .kind = headless ? UI_SURFACE_HEADLESS : UI_SURFACE_WINDOW,
            .width = 800,
            .height = 600,
            .title = "Axe — spring_rect",
        },
        .logic = demo_logic_desc(&logic),
        .start_paused = headless,
    };
    ui_app *app;
    ui_result r = ui_app_create(&desc, &app);
    if (r != UI_OK) {
        fprintf(stderr, "cannot start: %s\n", ui_result_str(r));
        demo_logic_fini(&logic);
        return 1;
    }
    int status;
    if (headless) {
        status = run_headless(app, seconds, out);
    } else {
        r = ui_app_run(app);
        status = r == UI_OK ? 0 : 1;
        if (r != UI_OK) fprintf(stderr, "run failed: %s\n", ui_result_str(r));
    }
    ui_app_destroy(app);
    demo_logic_fini(&logic);
    return status;
}
