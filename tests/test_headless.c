// Headless end-to-end test: the real demo logic, core and renderer on an offscreen surface
// at 1.25x, with a paused clock stepped to fixed times. Checks pixels at known coordinates,
// then runs the same script under the threaded scheduler and requires identical images.
#include <stdlib.h>
#include <string.h>

#include "core/spring.h"
#include "demo_logic.h"
#include "test.h"
#include "ui/ui.h"

#define WIDTH 320u
#define HEIGHT 200u
#define SCALE_120 150u // 1.25x
#define SCALE (SCALE_120 / 120.0)
#define MS 1000000ull

enum { SHOT_T0, SHOT_T2_1, SHOT_T4, SHOT_COUNT };

typedef struct rgb {
    int r, g, b;
} rgb;

static rgb pixel(const ui_image *img, uint32_t x, uint32_t y) {
    const uint8_t *p = img->pixels + (size_t)y * img->stride + 4u * x;
    return (rgb){p[0], p[1], p[2]};
}

static rgb expected_color(uint32_t anchor) {
    float c[4];
    demo_logic_color(anchor, c);
    return (rgb){(int)(c[0] * 255.f + 0.5f), (int)(c[1] * 255.f + 0.5f), (int)(c[2] * 255.f + 0.5f)};
}

static bool near_rgb(rgb a, rgb b, int tol) {
    return abs(a.r - b.r) <= tol && abs(a.g - b.g) <= tol && abs(a.b - b.b) <= tol;
}

#define CHECK_RGB(img, x, y, want, tol)                                                        \
    do {                                                                                       \
        rgb got_ = pixel((img), (x), (y)), want_ = (want);                                     \
        if (!near_rgb(got_, want_, (tol))) {                                                   \
            fprintf(stderr, "%s:%d: pixel (%u,%u) = (%d,%d,%d), want (%d,%d,%d)\n", __FILE__,  \
                    __LINE__, (unsigned)(x), (unsigned)(y), got_.r, got_.g, got_.b, want_.r,   \
                    want_.g, want_.b);                                                         \
            test_failures++;                                                                   \
        }                                                                                      \
    } while (0)

static bool frame(ui_app *app) {
    ui_result r = ui_app_frame(app);
    if (r != UI_OK) fprintf(stderr, "ui_app_frame: %s\n", ui_result_str(r));
    return r == UI_OK;
}

static bool shoot(ui_app *app, ui_image *out, const char *ppm) {
    ui_result r = ui_surface_capture(app, ui_app_main_surface(app), out);
    if (r != UI_OK) {
        fprintf(stderr, "capture: %s\n", ui_result_str(r));
        return false;
    }
    if (ppm && ui_image_write_ppm(out, ppm) != UI_OK) fprintf(stderr, "cannot write %s\n", ppm);
    return true;
}

// The script: settle at t=0, trigger the first spring at t=2 s, sample it in flight at
// t=2.1 s and settled at t=4 s.
static bool run_script(ui_scheduler_kind scheduler, ui_image shots[SHOT_COUNT], bool write_ppm,
                       ui_app_stats *stats) {
    demo_logic logic;
    if (demo_logic_init(&logic) != UI_OK) return false;
    logic.stall = false; // the stall flag is for the interactive example only
    ui_app_desc desc = {
        .scheduler = scheduler,
        .surface = {.kind = UI_SURFACE_HEADLESS, .width = WIDTH, .height = HEIGHT,
                    .scale_120 = SCALE_120},
        .logic = demo_logic_desc(&logic),
        .start_paused = true,
    };
    ui_app *app;
    ui_result r = ui_app_create(&desc, &app);
    if (r != UI_OK) {
        fprintf(stderr, "ui_app_create: %s (is a Vulkan driver installed? UI_GPU=%s)\n",
                ui_result_str(r), getenv("UI_GPU") ? getenv("UI_GPU") : "");
        demo_logic_fini(&logic);
        return false;
    }
    bool ok = frame(app) && frame(app) && // logic creates the node, then it is applied
              shoot(app, &shots[SHOT_T0], write_ppm ? "spring_rect_t0.ppm" : NULL);
    if (ok) {
        ui_app_clock_step(app, 2000 * MS);
        ok = frame(app) && frame(app); // logic sends the spring; the core starts it at t=2 s
    }
    if (ok) {
        ui_app_clock_step(app, 100 * MS);
        ok = frame(app) && shoot(app, &shots[SHOT_T2_1], write_ppm ? "spring_rect_t2_1.ppm" : NULL);
    }
    if (ok) {
        ui_app_clock_step(app, 1900 * MS);
        ok = frame(app) && shoot(app, &shots[SHOT_T4], write_ppm ? "spring_rect_t4.ppm" : NULL);
    }
    if (ok) CHECK_EQ_INT(ui_app_clock_now(app), 4000 * MS);
    ui_app_get_stats(app, stats);
    ui_app_destroy(app);
    demo_logic_fini(&logic);
    return ok;
}

static void check_shots(const ui_image shots[SHOT_COUNT], const ui_app_stats *stats) {
    float a[4], b[4];
    demo_logic_rect(0, WIDTH, HEIGHT, a);
    demo_logic_rect(1, WIDTH, HEIGHT, b);
    const ui_image *t0 = &shots[SHOT_T0];
    CHECK_EQ_INT(t0->width, 400);  // 320 * 1.25: rendered at physical size
    CHECK_EQ_INT(t0->height, 250);
    rgb bg = pixel(t0, 2, 2);
    CHECK(bg.r < 40 && bg.g < 40 && bg.b < 40);

    uint32_t ay = (uint32_t)((a[1] + a[3] / 2) * SCALE); // rect centers, physical px
    uint32_t ax = (uint32_t)((a[0] + a[2] / 2) * SCALE);
    uint32_t bx = (uint32_t)((b[0] + b[2] / 2) * SCALE);

    // t = 0: the rect sits at anchor 0.
    CHECK_RGB(t0, ax, ay, expected_color(0), 2);
    CHECK_RGB(t0, bx, ay, bg, 0);
    // Its top edge is at y = 30 * 1.25 = 37.5: the pixel row straddling it is half covered,
    // so it must be strictly between the background and the fill (anti-aliased).
    uint32_t edge_y = (uint32_t)(a[1] * SCALE);
    CHECK_NEAR(a[1] * SCALE - edge_y, 0.5, 1e-4);
    rgb half = pixel(t0, ax, edge_y), fill = expected_color(0);
    CHECK(half.r > bg.r + 30 && half.r < fill.r - 30);
    CHECK_RGB(t0, ax, edge_y - 1, bg, 1);
    CHECK_RGB(t0, ax, edge_y + 1, fill, 2);
    // The rounded corner leaves the bounding-box corner empty.
    CHECK_RGB(t0, (uint32_t)(a[0] * SCALE) + 1, edge_y + 1, bg, 2);

    // t = 2.1 s: 100 ms into the spring toward anchor 1. The left edge must be where the
    // closed-form spring says, within a pixel.
    const ui_image *t21 = &shots[SHOT_T2_1];
    double x, v;
    ui_spring_solve(DEMO_STIFFNESS, DEMO_DAMPING, a[0] - b[0], 0.0, 0.1, &x, &v);
    double left_expected = (b[0] + x) * SCALE;
    int left = -1;
    for (uint32_t px = 0; px < t21->width && left < 0; px++) {
        rgb p = pixel(t21, px, ay);
        if (abs(p.r - bg.r) + abs(p.g - bg.g) + abs(p.b - bg.b) > 120) left = (int)px; // >50%
    }
    fprintf(stderr, "in flight: left edge at %d px, spring predicts %.2f px\n", left, left_expected);
    CHECK(left > (int)(a[0] * SCALE) && left < (int)(b[0] * SCALE));
    CHECK_NEAR(left, left_expected, 1.5);

    // t = 4 s: settled at anchor 1 with its color; anchor 0 is empty.
    const ui_image *t4 = &shots[SHOT_T4];
    CHECK_RGB(t4, bx, ay, expected_color(1), 2);
    CHECK_RGB(t4, ax, ay, bg, 0);

    CHECK_EQ_INT(stats->commands_dropped, 0);
    CHECK_EQ_INT(stats->buffers_rejected, 0);
    CHECK_EQ_INT(stats->frames_committed, 6);
}

int main(void) {
    ui_image single[SHOT_COUNT] = {0}, threaded[SHOT_COUNT] = {0};
    ui_app_stats single_stats, threaded_stats;
    if (!run_script(UI_SCHEDULER_SINGLE, single, true, &single_stats)) return 1;
    check_shots(single, &single_stats);
    fprintf(stderr, "%s single scheduler\n", test_failures ? "FAIL" : "pass");

    if (!run_script(UI_SCHEDULER_THREADED, threaded, false, &threaded_stats)) return 1;
    int before = test_failures;
    for (int i = 0; i < SHOT_COUNT; i++) {
        CHECK_EQ_INT(threaded[i].width, single[i].width);
        CHECK_EQ_INT(threaded[i].height, single[i].height);
        size_t bytes = (size_t)single[i].stride * single[i].height;
        CHECK(threaded[i].pixels && memcmp(threaded[i].pixels, single[i].pixels, bytes) == 0);
    }
    CHECK_EQ_INT(threaded_stats.frames_committed, single_stats.frames_committed);
    fprintf(stderr, "%s threaded scheduler matches single\n", test_failures > before ? "FAIL" : "pass");

    for (int i = 0; i < SHOT_COUNT; i++) {
        ui_image_free(&single[i]);
        ui_image_free(&threaded[i]);
    }
    return test_failures ? 1 : 0;
}
