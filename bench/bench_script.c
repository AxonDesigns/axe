// Script benchmarks. Meaningful in release builds:
//   cmake --build --preset release && build/release/bench/bench_script
//  1. Encoding from JS: groups of the ops an app sends most (set_rect, set_color, set_props
//     with 3 entries, set_styles, a short set_text, create_node and destroy_node) written
//     through @axe/core; ops per second and MB per second.
//  2. Startup: the script host created and its first turn run (the bundle evaluated, the
//     layout_cards grid built and its commands flushed), from source and from bytecode.
//  3. Memory: the JS heap after startup, for an empty app and for layout_cards.
//  4. Event to handler: one turn carrying a frame_committed to a JS frame handler (and back:
//     the flush), and the per-event cost in a turn carrying 64.
//  5. Startup to first frame with a GPU: ui_app_create to the first frame showing the
//     layout_cards grid, from source and from bytecode.
// Every case prints its median and best run. Cases 1, 2 and 4 run on the CPU alone, each in a
// noinline bench_case_* function holding exactly what is timed: `bench_script --callgrind`
// runs them once each and skips the others, so Callgrind can count their instructions
// (tools/bench/callgrind.py).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/app_internal.h"
#include "script/host/host.h"
#include "ui/ui.h"
#include "wire/gen/wire_gen.h"

#define RUNS 9

static bool callgrind;
static ui_logger quiet = {UI_LOG_ERROR};
static ui_hooks hooks;

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void report(const char *what, uint64_t *ns, int n, double per, const char *unit) {
    uint64_t best = ns[0];
    for (int i = 1; i < n; i++) best = ns[i] < best ? ns[i] : best;
    qsort(ns, (size_t)n, sizeof *ns, cmp_u64);
    printf("  %-44s median %9.1f us  best %9.1f us", what, (double)ns[n / 2] / 1e3, (double)best / 1e3);
    if (per > 0) printf("  (%.1f %s at best)", per / ((double)best / 1e9), unit);
    printf("\n");
}

static ui_script_host *host(const char *path) {
    // No long-task reports: under Callgrind every task is long, and reporting would count.
    ui_script_desc d = {.bundle_path = path, .long_task_ns = UINT64_MAX / 2};
    ui_script_env env = {.log = &quiet, .hooks = &hooks, .start_ns = ui_monotonic_ns()};
    ui_script_host *h = NULL;
    if (ui_script_host_create(&d, &env, &h) != UI_OK) {
        fprintf(stderr, "can't start the host with %s\n", path);
        exit(1);
    }
    return h;
}

static const ui_handle surface = {(uint32_t)UI_HANDLE_SURFACE << UI_HANDLE_SLOT_BITS, 1};

// An event buffer: the clock, then the first turn's surface_resized or `frames` frames.
static uint8_t *events(uint64_t now, bool resized, int frames, size_t *size) {
    ui_wire_buf b;
    ui_wire_buf_init(&b, UI_WIRE_MAGIC_EVENT);
    ui_wire_clock_put(&b, &(ui_wire_clock){now});
    if (resized) ui_wire_surface_resized_put(&b, &(ui_wire_surface_resized){surface, 980, 640, 150});
    for (int i = 0; i < frames; i++) ui_wire_frame_committed_put(&b, &(ui_wire_frame_committed){surface, 1, now});
    return ui_wire_buf_finish(&b, size);
}

static void turn(ui_script_host *h, const uint8_t *ev, size_t size) {
    uint8_t *cmds;
    size_t n;
    ui_script_host_turn(h, ev, size, &cmds, &n);
    free(cmds);
}

static double call_number(ui_script_host *h, const char *fn, double arg) {
    ui_js *js = ui_script_host_engine(h);
    uint32_t scope = ui_js_scope_open(js);
    ui_js_val f, r;
    double out = -1;
    if (ui_js_get(js, ui_js_global(js), fn, &f) && ui_js_call(js, f, ui_js_undefined(js), 1, (ui_js_val[]){ui_js_number(js, arg)}, &r))
        ui_js_to_number(js, r, &out);
    ui_js_scope_close(js, scope);
    return out;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double call_kind(ui_script_host *h, int kind, int n) {
    ui_js *js = ui_script_host_engine(h);
    uint32_t scope = ui_js_scope_open(js);
    ui_js_val f, r;
    double out = -1;
    if (ui_js_get(js, ui_js_global(js), "benchKind", &f) &&
        ui_js_call(js, f, ui_js_undefined(js), 2, (ui_js_val[]){ui_js_number(js, kind), ui_js_number(js, n)}, &r))
        ui_js_to_number(js, r, &out);
    ui_js_scope_close(js, scope);
    return out;
}

// ---- 1. Encoding -----------------------------------------------------------------------------

#define GROUPS 2000
#define OPS_PER_GROUP 7

__attribute__((noinline)) static double bench_case_script_encode(ui_script_host *h) {
    return call_number(h, "benchEncode", GROUPS);
}

static void bench_encode(void) {
    ui_script_host *h = host(UI_BENCH_BUNDLE);
    size_t size;
    uint8_t *ev = events(0, true, 0, &size);
    turn(h, ev, size); // the app's nodes
    free(ev);
    if (callgrind) {
        bench_case_script_encode(h);
    } else {
        uint64_t ns[RUNS];
        double bytes = 0;
        call_number(h, "benchEncode", GROUPS); // warm up
        for (int i = 0; i < RUNS; i++) {
            uint64_t t0 = ui_monotonic_ns();
            bytes = bench_case_script_encode(h);
            ns[i] = ui_monotonic_ns() - t0;
        }
        printf("Encoding from JS (%d groups of %d ops, %.0f bytes):\n", GROUPS, OPS_PER_GROUP, bytes);
        uint64_t copy[RUNS];
        memcpy(copy, ns, sizeof copy);
        report("ops", ns, RUNS, (double)GROUPS * OPS_PER_GROUP / 1e6, "Mops/s");
        report("bytes", copy, RUNS, bytes / 1e6, "MB/s");
        static const char *const kinds[] = {"node.setRect", "node.setColor", "node.setProps (3 entries)",
                                            "node.setStyles (2 styles)", "node.setText (\"Card N\")",
                                            "Node.create + destroy", "CommandWriter.setRect (@axe/wire)"};
        printf("  per op, median of %d runs of 5000:\n", RUNS);
        for (int k = 0; k < 7; k++) {
            double us[RUNS];
            for (int r = 0; r < RUNS; r++) us[r] = call_kind(h, k, 5000);
            qsort(us, RUNS, sizeof *us, cmp_double);
            printf("    %-40s %6.2f us\n", kinds[k], us[RUNS / 2]);
        }
    }
    ui_script_host_destroy(h);
}

// ---- 2 and 3. Startup and memory ---------------------------------------------------------------

__attribute__((noinline)) static ui_script_host *bench_case_startup_source(const uint8_t *ev, size_t size) {
    ui_script_host *h = host(UI_CARDS_JS);
    turn(h, ev, size);
    return h;
}

__attribute__((noinline)) static ui_script_host *bench_case_startup_bytecode(const uint8_t *ev, size_t size) {
    ui_script_host *h = host(UI_CARDS_AXBC);
    turn(h, ev, size);
    return h;
}

static uint64_t heap(ui_script_host *h) {
    ui_js_memory m;
    ui_js_memory_stats(ui_script_host_engine(h), &m);
    return m.used_bytes;
}

static void bench_startup(void) {
    size_t size;
    uint8_t *ev = events(0, true, 0, &size);
    if (callgrind) {
        ui_script_host_destroy(bench_case_startup_source(ev, size));
        ui_script_host_destroy(bench_case_startup_bytecode(ev, size));
        free(ev);
        return;
    }
    uint64_t src[RUNS], bc[RUNS], mem_cards = 0;
    for (int i = 0; i < RUNS; i++) {
        uint64_t t0 = ui_monotonic_ns();
        ui_script_host *h = bench_case_startup_source(ev, size);
        src[i] = ui_monotonic_ns() - t0;
        ui_script_host_destroy(h);
        t0 = ui_monotonic_ns();
        h = bench_case_startup_bytecode(ev, size);
        bc[i] = ui_monotonic_ns() - t0;
        mem_cards = heap(h);
        ui_script_host_destroy(h);
    }
    printf("Startup: host created, layout_cards evaluated and its first turn flushed:\n");
    report("from source", src, RUNS, 0, "");
    report("from bytecode", bc, RUNS, 0, "");

    ui_script_host *empty = host(UI_EMPTY_BUNDLE);
    turn(empty, ev, size);
    printf("Memory: JS heap after the first turn:\n");
    printf("  %-44s %9.1f KiB\n", "an empty app", (double)heap(empty) / 1024);
    printf("  %-44s %9.1f KiB\n", "layout_cards (10 cards)", (double)mem_cards / 1024);
    ui_script_host_destroy(empty);
    free(ev);
}

// ---- 4. Event to handler ---------------------------------------------------------------------

#define TURNS 1000
#define BATCH 64

__attribute__((noinline)) static void bench_case_script_dispatch(ui_script_host *h, uint8_t **ev, const size_t *size,
                                                                int turns) {
    for (int i = 0; i < turns; i++) turn(h, ev[i], size[i]);
}

static void bench_dispatch(void) {
    ui_script_host *h = host(UI_BENCH_BUNDLE);
    size_t size;
    uint8_t *first = events(0, true, 0, &size);
    turn(h, first, size);
    free(first);
    // The clock moves 16 ms per turn; buffers are made beforehand so only the turn is timed.
    uint8_t **ev = malloc(TURNS * sizeof *ev), **batch = malloc(TURNS * sizeof *batch);
    size_t *sizes = malloc(TURNS * sizeof *sizes), *batch_sizes = malloc(TURNS * sizeof *batch_sizes);
    for (int i = 0; i < TURNS; i++) {
        ev[i] = events((uint64_t)(i + 1) * 16000000, false, 1, &sizes[i]);
        batch[i] = events((uint64_t)(i + 1) * 16000000, false, BATCH, &batch_sizes[i]);
    }
    if (callgrind) {
        bench_case_script_dispatch(h, ev, sizes, TURNS);
    } else {
        uint64_t one[RUNS], many[RUNS];
        for (int r = 0; r < RUNS; r++) {
            uint64_t t0 = ui_monotonic_ns();
            bench_case_script_dispatch(h, ev, sizes, TURNS);
            one[r] = (ui_monotonic_ns() - t0) / TURNS;
            t0 = ui_monotonic_ns();
            bench_case_script_dispatch(h, batch, batch_sizes, TURNS);
            many[r] = (ui_monotonic_ns() - t0) / (TURNS * BATCH);
        }
        printf("Event to handler (frame_committed to a JS onFrame handler; %.0f calls):\n", call_number(h, "benchFrames", 0));
        report("a turn of one event (per turn)", one, RUNS, 0, "");
        report("a turn of 64 events (per event)", many, RUNS, 0, "");
    }
    for (int i = 0; i < TURNS; i++) free(ev[i]), free(batch[i]);
    free(ev), free(batch), free(sizes), free(batch_sizes);
    ui_script_host_destroy(h);
}

// ---- 5. Startup to first frame ----------------------------------------------------------------

static uint64_t first_frame(const char *bundle) {
    static const char *const fonts[] = {AXE_SOURCE_DIR "/tests/fonts/NotoSans-VF.ttf"};
    ui_script_desc script = {.bundle_path = bundle};
    ui_app_desc desc = {
        .surface = {.kind = UI_SURFACE_HEADLESS, .width = 980, .height = 640, .scale_120 = 150},
        .script = &script,
        .start_paused = true,
        .text = {.font_files = fonts, .font_file_count = 1},
    };
    uint64_t t0 = ui_monotonic_ns();
    ui_app *app;
    if (ui_app_create(&desc, &app) != UI_OK) return 0;
    ui_app_stats stats = {0};
    for (int i = 0; i < 100 && !stats.commands_applied; i++) {
        ui_app_frame(app);
        ui_app_get_stats(app, &stats);
    }
    uint64_t took = ui_monotonic_ns() - t0;
    ui_app_destroy(app);
    return took;
}

static void bench_first_frame(void) {
    uint64_t src[5], bc[5];
    for (int i = 0; i < 5; i++) {
        src[i] = first_frame(UI_CARDS_JS);
        bc[i] = first_frame(UI_CARDS_AXBC);
    }
    printf("Startup to first frame (ui_app_create to the frame drawing the grid, with a GPU):\n");
    report("from source", src, 5, 0, "");
    report("from bytecode", bc, 5, 0, "");
}

int main(int argc, char **argv) {
    callgrind = argc > 1 && !strcmp(argv[1], "--callgrind");
    ui_hooks_init(&hooks);
    bench_encode();
    bench_startup();
    bench_dispatch();
    if (!callgrind) bench_first_frame();
    return 0;
}
