// The language benchmarks (docs/ui.md, "Benchmarks"; thresholds agreed in milestone 3b):
// TSX apps on @axe/ui, the JS side of each turn timed on the CPU. Meaningful in release builds:
//   cmake --build --preset release && build/release/bench/bench_ui
//
//  B1. bench/apps/screen1k (about 1,000 nodes): the host created from bytecode, the app evaluated
//      and its first turn flushed; and, with a GPU, ui_app_create to its first frame, against an
//      empty app's.
//  B2. bench/apps/list10k: 10,000 rows mounted through For (the first turn), the JS heap per row;
//      then 600 turns each writing 100 items' signals (p50, p99), and turns moving 50 rows.
//  B3. bench/apps/text1k: 1,000 bound texts, all updated in one turn.
//
// A turn here is what the logic thread does: the app's writes (a call into the app), then the
// turn (events, the effect flush and the buffer). Its commands go nowhere: the core's side is
// measured elsewhere (bench_layout, bench_text). `bench_ui --callgrind` runs each CPU case once
// for tools/bench/callgrind.py.
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

static uint64_t pct(uint64_t *ns, int n, int p) {
    qsort(ns, (size_t)n, sizeof *ns, cmp_u64);
    int i = (int)((int64_t)(n - 1) * p / 100);
    return ns[i];
}

static void report(const char *what, uint64_t *ns, int n) {
    uint64_t best = ns[0];
    for (int i = 1; i < n; i++) best = ns[i] < best ? ns[i] : best;
    printf("  %-48s median %9.2f ms  best %9.2f ms\n", what, (double)pct(ns, n, 50) / 1e6, (double)best / 1e6);
}

static ui_script_host *host(const char *path) {
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

static uint8_t *events(uint64_t now, bool resized, size_t *size) {
    ui_wire_buf b;
    ui_wire_buf_init(&b, UI_WIRE_MAGIC_EVENT);
    ui_wire_clock_put(&b, &(ui_wire_clock){now});
    if (resized) ui_wire_surface_resized_put(&b, &(ui_wire_surface_resized){surface, 980, 640, 150});
    return ui_wire_buf_finish(&b, size);
}

static size_t turn(ui_script_host *h, const uint8_t *ev, size_t size) {
    uint8_t *cmds;
    size_t n;
    ui_script_host_turn(h, ev, size, &cmds, &n);
    free(cmds);
    return n;
}

static void call(ui_script_host *h, const char *fn, double arg) {
    ui_js *js = ui_script_host_engine(h);
    uint32_t scope = ui_js_scope_open(js);
    ui_js_val f, r;
    if (!ui_js_get(js, ui_js_global(js), fn, &f) || !ui_js_call(js, f, ui_js_undefined(js), 1, (ui_js_val[]){ui_js_number(js, arg)}, &r)) {
        fprintf(stderr, "%s failed\n", fn);
        exit(1);
    }
    ui_js_scope_close(js, scope);
}

static uint64_t heap(ui_script_host *h) {
    ui_js_memory m;
    ui_js_memory_stats(ui_script_host_engine(h), &m);
    return m.used_bytes;
}

// The host with an app, and its first turn (the app evaluated, mounted, flushed).
static ui_script_host *started(const char *bundle) {
    size_t size;
    uint8_t *ev = events(0, true, &size);
    ui_script_host *h = host(bundle);
    turn(h, ev, size);
    free(ev);
    return h;
}

// One turn of the app's `fn(k)`, at 16 ms a turn.
static void update_turn(ui_script_host *h, const char *fn, int k) {
    size_t size;
    uint8_t *ev = events((uint64_t)(k + 1) * 16000000, false, &size);
    call(h, fn, k);
    turn(h, ev, size);
    free(ev);
}

// ---- Cases ----------------------------------------------------------------------------------------

__attribute__((noinline)) static ui_script_host *bench_case_ui_startup_1k(void) { return started(UI_SCREEN1K_AXBC); }

__attribute__((noinline)) static ui_script_host *bench_case_ui_for_mount_10k(void) { return started(UI_LIST10K_AXBC); }

__attribute__((noinline)) static void bench_case_ui_for_update(ui_script_host *h, int first, int turns) {
    for (int k = first; k < first + turns; k++) update_turn(h, "benchUpdate", k);
}

__attribute__((noinline)) static void bench_case_ui_for_reorder(ui_script_host *h, int k) { update_turn(h, "benchReorder", k); }

__attribute__((noinline)) static void bench_case_ui_text_1k(ui_script_host *h, int k) { update_turn(h, "benchUpdate", k); }

// ---- B1 -------------------------------------------------------------------------------------------

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
    for (int i = 0; i < 200 && !stats.commands_applied; i++) {
        ui_app_frame(app);
        ui_app_get_stats(app, &stats);
    }
    // The frame after the commands applied draws them.
    ui_app_frame(app);
    uint64_t took = ui_monotonic_ns() - t0;
    ui_app_destroy(app);
    return took;
}

static void b1(void) {
    uint64_t js[RUNS];
    uint64_t mem = 0;
    for (int i = 0; i < RUNS; i++) {
        uint64_t t0 = ui_monotonic_ns();
        ui_script_host *h = bench_case_ui_startup_1k();
        js[i] = ui_monotonic_ns() - t0;
        mem = heap(h);
        ui_script_host_destroy(h);
    }
    printf("B1. screen1k (about 1,000 nodes) from bytecode:\n");
    report("JS: host, evaluation, mount and first flush", js, RUNS);
    printf("  %-48s %9.1f KiB\n", "JS heap after the first turn", (double)mem / 1024);
    uint64_t app[5], empty[5];
    for (int i = 0; i < 5; i++) {
        empty[i] = first_frame(UI_EMPTY_AXBC);
        app[i] = first_frame(UI_SCREEN1K_AXBC);
    }
    report("first frame: the empty app", empty, 5);
    report("first frame: screen1k", app, 5);
}

// ---- B2 -------------------------------------------------------------------------------------------

#define UPDATE_TURNS 600

static void b2(void) {
    uint64_t mount[5];
    uint64_t mem = 0;
    ui_script_host *empty = started(UI_EMPTY_AXBC);
    uint64_t base = heap(empty);
    ui_script_host_destroy(empty);
    for (int i = 0; i < 5; i++) {
        uint64_t t0 = ui_monotonic_ns();
        ui_script_host *h = bench_case_ui_for_mount_10k();
        mount[i] = ui_monotonic_ns() - t0;
        mem = heap(h);
        ui_script_host_destroy(h);
    }
    printf("B2. list10k (10,000 rows through For):\n");
    report("mount: host, evaluation, 10,000 rows, flush", mount, 5);
    printf("  %-48s %9.0f bytes (%.1f MiB in all)\n", "JS heap per row", (double)(mem - base) / 10000,
           (double)(mem - base) / (1 << 20));
    ui_script_host *h = started(UI_LIST10K_AXBC);
    static uint64_t turns[UPDATE_TURNS];
    for (int k = 0; k < UPDATE_TURNS; k++) {
        uint64_t t0 = ui_monotonic_ns();
        bench_case_ui_for_update(h, k, 1);
        turns[k] = ui_monotonic_ns() - t0;
    }
    uint64_t p50 = pct(turns, UPDATE_TURNS, 50), p99 = pct(turns, UPDATE_TURNS, 99), max = pct(turns, UPDATE_TURNS, 100);
    printf("  %-48s p50 %6.2f ms  p99 %6.2f ms  max %6.2f ms\n", "a turn writing 100 items (600 turns)", (double)p50 / 1e6,
           (double)p99 / 1e6, (double)max / 1e6);
    uint64_t reorder[RUNS];
    for (int k = 0; k < RUNS; k++) {
        uint64_t t0 = ui_monotonic_ns();
        bench_case_ui_for_reorder(h, k);
        reorder[k] = ui_monotonic_ns() - t0;
    }
    report("a turn moving 50 of 10,000 rows", reorder, RUNS);
    ui_script_host_destroy(h);
}

// ---- B3 -------------------------------------------------------------------------------------------

static void b3(void) {
    ui_script_host *h = started(UI_TEXT1K_AXBC);
    uint64_t ns[RUNS * 3];
    for (int k = 0; k < RUNS * 3; k++) {
        uint64_t t0 = ui_monotonic_ns();
        bench_case_ui_text_1k(h, k);
        ns[k] = ui_monotonic_ns() - t0;
    }
    printf("B3. text1k:\n");
    report("a turn updating 1,000 bound texts", ns, RUNS * 3);
    ui_script_host_destroy(h);
}

int main(int argc, char **argv) {
    callgrind = argc > 1 && !strcmp(argv[1], "--callgrind");
    // bench_ui --heap BUNDLE...: the JS heap after each bundle's first turn (an empty app's
    // first), for taking a row's cost apart.
    if (argc > 2 && !strcmp(argv[1], "--heap")) {
        ui_hooks_init(&hooks);
        ui_script_host *e = started(UI_EMPTY_AXBC);
        uint64_t base = heap(e);
        ui_script_host_destroy(e);
        for (int i = 2; i < argc; i++) {
            ui_script_host *h = started(argv[i]);
            printf("%-60s %10.1f KiB over an empty app\n", argv[i], (double)(heap(h) - base) / 1024);
            ui_script_host_destroy(h);
        }
        return 0;
    }
    ui_hooks_init(&hooks);
    if (callgrind) {
        ui_script_host_destroy(bench_case_ui_startup_1k());
        ui_script_host *h = bench_case_ui_for_mount_10k();
        bench_case_ui_for_update(h, 0, 10);
        bench_case_ui_for_reorder(h, 0);
        ui_script_host_destroy(h);
        h = started(UI_TEXT1K_AXBC);
        bench_case_ui_text_1k(h, 0);
        ui_script_host_destroy(h);
        return 0;
    }
    b1();
    b2();
    b3();
    return 0;
}
