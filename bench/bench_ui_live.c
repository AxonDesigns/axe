// B2's live part (docs/ui.md, "Benchmarks"): bench/apps/list10k in a window, writing 100 items'
// signals on every frame, for some seconds; then
//  - dropped frames: refreshes with no new frame, from the compositor's present times
//    (the hook API's PRESENTED);
//  - update delay: from the logic thread sending a turn's commands to the present of the frame
//    that applied them (COMMANDS_APPLIED with PRESENTED), in frames.
// The first two seconds (the mount, the first shaping) are left out.
//
//   bench_ui_live [--seconds S] [--threaded|--single] [--rows=N] [bundle.axbc]
// Run it in a compositor with a fixed refresh rate (a headless sway: docs/ui.md).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/common.h"
#include "ui/ui.h"

#define MAX_EVENTS 200000

typedef struct sample {
    uint64_t frame, t;
} sample;

static struct {
    ui_app *app;
    uint64_t start, seconds;
    sample presented[MAX_EVENTS], applied[MAX_EVENTS];
    uint32_t presented_count, applied_count;
    uint64_t refresh;
} run;

static void on_trace(void *user, const ui_trace_event *e) {
    (void)user;
    if (e->kind == UI_TRACE_PRESENTED && run.presented_count < MAX_EVENTS) {
        run.presented[run.presented_count++] = (sample){e->frame_id, e->time_ns};
        if (e->refresh_ns) run.refresh = e->refresh_ns;
        if (e->time_ns > run.start + run.seconds * 1000000000ull) ui_app_quit(run.app);
    } else if (e->kind == UI_TRACE_COMMANDS_APPLIED && e->time_ns && run.applied_count < MAX_EVENTS) {
        run.applied[run.applied_count++] = (sample){e->frame_id, e->time_ns};
    }
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv) {
    const char *bundle = UI_LIST10K_AXBC;
    run.seconds = 12;
    ui_scheduler_kind scheduler = UI_SCHEDULER_THREADED;
    const char *args[2] = {"--stream", NULL};
    uint32_t arg_count = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) run.seconds = (uint64_t)atoi(argv[++i]);
        else if (!strncmp(argv[i], "--rows=", 7)) args[arg_count++] = argv[i];
        else if (!strcmp(argv[i], "--single")) scheduler = UI_SCHEDULER_SINGLE;
        else if (!strcmp(argv[i], "--threaded")) scheduler = UI_SCHEDULER_THREADED;
        else bundle = argv[i];
    }
    static const char *const fonts[] = {AXE_SOURCE_DIR "/tests/fonts/NotoSans-VF.ttf"};
    ui_script_desc script = {.bundle_path = bundle, .args = args, .arg_count = arg_count, .long_task_ns = UINT64_MAX / 2};
    ui_app_desc desc = {
        .scheduler = scheduler,
        .surface = {.kind = UI_SURFACE_WINDOW, .width = 800, .height = 600, .title = "bench_ui_live"},
        .script = &script,
        .text = {.font_files = fonts, .font_file_count = 1},
    };
    if (ui_app_create(&desc, &run.app) != UI_OK) {
        fprintf(stderr, "can't create the app (a Wayland compositor is needed)\n");
        return 1;
    }
    uint32_t id;
    ui_hooks_subscribe(run.app, on_trace, NULL, &id);
    run.start = ui_monotonic_ns();
    ui_app_run(run.app);
    ui_app_destroy(run.app);

    uint64_t from = run.start + 2000000000ull;
    if (!run.refresh || run.presented_count < 10) {
        fprintf(stderr, "no present times (the compositor gave %u, refresh %llu ns)\n", run.presented_count,
                (unsigned long long)run.refresh);
        return 1;
    }
    // Dropped frames: each gap between consecutive presents counts its missed refreshes.
    uint64_t frames = 0, missed = 0;
    for (uint32_t i = 1; i < run.presented_count; i++) {
        if (run.presented[i - 1].t < from) continue;
        uint64_t gap = run.presented[i].t - run.presented[i - 1].t;
        uint64_t refreshes = (gap + run.refresh / 2) / run.refresh;
        frames++;
        if (refreshes > 1) missed += refreshes - 1;
    }
    // Update delay: each applied buffer's frame's present, minus when it was sent.
    static uint64_t delays[MAX_EVENTS];
    uint32_t n = 0, j = 0;
    for (uint32_t i = 0; i < run.applied_count; i++) {
        if (run.applied[i].t < from) continue;
        while (j < run.presented_count && run.presented[j].frame < run.applied[i].frame) j++;
        if (j < run.presented_count && run.presented[j].frame == run.applied[i].frame && run.presented[j].t > run.applied[i].t)
            delays[n++] = run.presented[j].t - run.applied[i].t;
    }
    double hz = 1e9 / (double)run.refresh;
    printf("B2 live: list10k%s%s streaming 100 items a frame, %llu s at %.1f Hz (%s scheduler)\n",
           arg_count > 1 ? " " : "", arg_count > 1 ? args[1] : "", (unsigned long long)run.seconds - 2, hz,
           scheduler == UI_SCHEDULER_SINGLE ? "single" : "threaded");
    printf("  frames presented %llu, refreshes missed %llu (%.2f%%)\n", (unsigned long long)frames,
           (unsigned long long)missed, 100.0 * (double)missed / (double)(frames + missed));
    if (n) {
        qsort(delays, n, sizeof *delays, cmp_u64);
        uint64_t p50 = delays[n / 2], p99 = delays[(uint32_t)((uint64_t)(n - 1) * 99 / 100)];
        printf("  update delay over %u buffers: p50 %.1f ms (%.2f frames), p99 %.1f ms (%.2f frames)\n", n,
               (double)p50 / 1e6, (double)p50 / (double)run.refresh, (double)p99 / 1e6, (double)p99 / (double)run.refresh);
    }
    return 0;
}
