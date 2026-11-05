// Hot reload's latency (docs/ui.md, "Hot reload"): from the file event to the first frame after
// the update's flush, for 50 scripted edits of each kind. Needs AXE_DEV and a compositor:
//
//   WAYLAND_DISPLAY=<headless sway> build/release-dev/bench/bench_hot [--edits N] [--dir D]
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>

#include "runtime/app_internal.h"
#include "runtime/common.h"
#include "script/host/host.h"
#include "session.h"
#include "ui/hooks.h"
#include "ui/ui.h"

#define MAX_EDITS 256

static const char *const FILES[] = {"main.tsx", "panel.tsx", "styles.ts", "util.ts"};
static const struct {
    const char *name, *file, *marker;
} KINDS[] = {
    {"component", "panel.tsx", "__COMPONENT__"},
    {"style only", "styles.ts", "__STYLE__"},
    {"template", "panel.tsx", "__TEMPLATE__"},
    {"non-component module", "util.ts", "__MODULE__"},
};
#define KIND_COUNT 4

static struct {
    ui_app *app;
    atomic_ullong update_event, update_flushed, target_frame, latency;
    atomic_uint updates;
} run;

static void on_trace(void *user, const ui_trace_event *e) {
    (void)user;
    if (e->kind == UI_TRACE_SCRIPT_HOT_UPDATE) {
        uint64_t now = ui_monotonic_ns();
        atomic_store(&run.target_frame, 0);
        atomic_store(&run.update_flushed, now);
        atomic_store(&run.update_event, e->duration_ns ? now - e->duration_ns : 0);
        atomic_fetch_add(&run.updates, 1);
    } else if (e->kind == UI_TRACE_COMMANDS_APPLIED) {
        uint64_t flushed = atomic_load(&run.update_flushed);
        if (flushed && e->time_ns >= flushed && !atomic_load(&run.target_frame)) atomic_store(&run.target_frame, e->frame_id);
    } else if (e->kind == UI_TRACE_FRAME_END) {
        uint64_t frame = atomic_load(&run.target_frame), event = atomic_load(&run.update_event);
        if (frame && event && e->frame_id >= frame) {
            atomic_store(&run.latency, e->time_ns - event);
            atomic_store(&run.update_event, 0);
            atomic_store(&run.target_frame, 0);
            atomic_store(&run.update_flushed, 0);
        }
    }
}

static char *read_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    if (fread(s, 1, (size_t)n, f) != (size_t)n) n = 0;
    s[n] = 0;
    fclose(f);
    return s;
}

// `path`, with each __X__ replaced by `value` for `marker` and by 1 for the others.
static void write_version(const char *from, const char *to, const char *marker, int value) {
    char *text = read_text(from);
    if (!text) {
        fprintf(stderr, "can't read %s\n", from);
        exit(1);
    }
    FILE *f = fopen(to, "wb");
    for (const char *p = text; *p;) {
        const char *m = strstr(p, "__");
        const char *end = m ? strstr(m + 2, "__") : NULL;
        if (!m || !end) {
            fputs(p, f);
            break;
        }
        fwrite(p, 1, (size_t)(m - p), f);
        size_t len = (size_t)(end + 2 - m);
        fprintf(f, "%d", marker && !strncmp(m, marker, len) ? value : 1);
        p = end + 2;
    }
    fclose(f);
    free(text);
}

static void sleep_ms(long ms) { thrd_sleep(&(struct timespec){.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L}, NULL); }

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static struct {
    const char *root;
    int edits;
    int failed;
} config;

static int editor(void *arg) {
    (void)arg;
    // The app comes up first.
    for (int i = 0; i < 2000 && !atomic_load(&run.updates); i++) sleep_ms(10);
    sleep_ms(500);
    static uint64_t samples[KIND_COUNT][MAX_EDITS];
    char from[4096], to[4096];
    for (int k = 0; k < KIND_COUNT; k++) {
        snprintf(from, sizeof from, "%s/%s", UI_HOT_APP_DIR, KINDS[k].file);
        snprintf(to, sizeof to, "%s/%s", config.root, KINDS[k].file);
        int n = 0;
        for (int i = 0; i < config.edits; i++) {
            atomic_store(&run.latency, 0);
            write_version(from, to, KINDS[k].marker, 100 + i);
            uint64_t latency = 0;
            for (int w = 0; w < 3000 && !(latency = atomic_load(&run.latency)); w++) sleep_ms(1);
            if (latency) samples[k][n++] = latency;
            else config.failed++;
            sleep_ms(40); // past the debounce, as a person saving is
        }
        qsort(samples[k], (size_t)n, sizeof **samples, cmp_u64);
        if (n) {
            printf("%-22s %3d edits: p50 %6.1f ms  p95 %6.1f ms  max %6.1f ms\n", KINDS[k].name, n,
                   (double)samples[k][n / 2] / 1e6, (double)samples[k][n * 95 / 100] / 1e6, (double)samples[k][n - 1] / 1e6);
            fflush(stdout);
        }
    }
    ui_app_quit(run.app);
    return 0;
}

static void wake_core(void *user) { ui_app_request_turn(user); }

int main(int argc, char **argv) {
    config.edits = 50;
    config.root = "/tmp/axe-bench-hot";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--edits") && i + 1 < argc) config.edits = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dir") && i + 1 < argc) config.root = argv[++i];
    }
    if (config.edits > MAX_EDITS) config.edits = MAX_EDITS;
    char command[8300], work[4096 + 8];
    snprintf(work, sizeof work, "%s-work", config.root);
    snprintf(command, sizeof command, "rm -rf '%s' '%s' && mkdir -p '%s'", config.root, work, config.root);
    if (system(command) != 0) return 1;
    char from[4096], to[4096];
    for (size_t i = 0; i < sizeof FILES / sizeof FILES[0]; i++) {
        snprintf(from, sizeof from, "%s/%s", UI_HOT_APP_DIR, FILES[i]);
        snprintf(to, sizeof to, "%s/%s", config.root, FILES[i]);
        write_version(from, to, NULL, 0);
    }
    ui_dev_channel *channel;
    if (ui_dev_channel_create(&(ui_dev_desc){.entry = "main.tsx", .packages = AXE_DEV_PACKAGES}, &channel) != UI_OK) return 1;
    ui_script_internal internal = {.dev = channel};
    ui_script_desc script = {.internal = &internal, .long_task_ns = UINT64_MAX / 2};
    static const char *const fonts[] = {AXE_DEV_SOURCE_DIR "/tests/fonts/NotoSans-VF.ttf"};
    ui_app_desc desc = {
        .scheduler = UI_SCHEDULER_THREADED,
        .surface = {.kind = UI_SURFACE_WINDOW, .width = 640, .height = 800, .title = "bench_hot"},
        .script = &script,
        .text = {.font_files = fonts, .font_file_count = 1},
    };
    if (ui_app_create(&desc, &run.app) != UI_OK) {
        fprintf(stderr, "can't create the app (a Wayland compositor is needed)\n");
        return 1;
    }
    uint32_t id;
    ui_hooks_subscribe(run.app, on_trace, NULL, &id);
    ui_dev_channel_set_wake(channel, wake_core, run.app);
    ui_dev_build_desc build = {config.root, work, AXE_TSX_PATH, AXE_ESBUILD_PATH, AXE_DEV_SOURCE_DIR};
    ui_dev_session *session = ui_dev_session_start(&build, channel);
    thrd_t thread;
    thrd_create(&thread, editor, NULL);
    ui_app_run(run.app);
    thrd_join(thread, NULL);
    ui_dev_session_stop(session);
    ui_app_destroy(run.app);
    ui_dev_channel_destroy(channel);
    if (config.failed) printf("%d edits timed out\n", config.failed);
    return config.failed ? 1 : 0;
}
