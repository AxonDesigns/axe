// axe-test-js: runs a TypeScript test bundle inside the script host, headless, and reports to
// CTest through its exit status (docs/typescript.md, "Tests").
//
//   axe-test-js bundle.js [--scheduler single|threaded]
//
// The app has a paused clock, and performance.now() reads it, so tests are deterministic.
// The bundle gets the module `axe:test` (@axe/testing wraps it):
//   requestStep(ms)  the runner applies what the turn wrote, then steps the clock by ms
//   done(failed, total)
//   hooks()          the script hooks reported so far: [{kind, text}]
//   gc()             a full collection now (FinalizationRegistry callbacks follow)
//   cBuffer(name)    a command buffer the C writer encodes (the encoder's reference cases)
//   cEvents()        an event buffer of every event, encoded by the C codec
//   hotEdit({path: text | null})  AXE_DEV: writes or deletes the app's files, rebuilds them and
//                    posts the result (--dev)
//   hotSave(path, text, how)  AXE_DEV: saves a file the way an editor does, for the watcher (how:
//                    "write", "rename", "recreate", or "delete" with no text)
//   hotWatching()    AXE_DEV: the app started under the watcher (--watch)
//   capture(path)    writes the surface as a PPM after the next frame
//   readText(path)   a file of the repository, as text
//   heapUsed()       the JS heap's size in bytes
// Without requests the runner keeps making frames (preparation, font loads); a test that
// neither steps nor finishes within the frame budget fails.
//
//   axe-test-js --dev DIR [--entry main.tsx] driver.js
// runs driver.js as a module next to an app of hot-reloaded modules, in DIR/src (emptied first).
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#ifdef AXE_DEV
#include <sys/stat.h>
#include <unistd.h>

#include "build.h"
#include "runtime/app_internal.h"
#include "session.h"
#endif
#include "script/host/host.h"
#include "ui/ui.h"
#include "wire/reference.h"

#define MAX_FRAMES 20000
#define MAX_HOOKS 256

typedef struct runner {
    mtx_t lock;
    atomic_ullong step_ns; // requested, not yet applied
    atomic_bool done;
    int failed, total;
    struct {
        ui_trace_kind kind;
        char *text;
    } hooks[MAX_HOOKS];
    uint32_t hook_count;
    _Atomic(char *) capture; // a path, malloc'd
#ifdef AXE_DEV
    ui_dev_channel *channel;
    ui_dev_build_desc build;
#endif
} runner;

static void on_trace(void *user, const ui_trace_event *ev) {
    if (ev->kind < UI_TRACE_SCRIPT_EXCEPTION || ev->kind > UI_TRACE_SCRIPT_LEAK) return; // script events only
    runner *r = user;
    mtx_lock(&r->lock);
    if (r->hook_count < MAX_HOOKS) {
        r->hooks[r->hook_count].kind = ev->kind;
        r->hooks[r->hook_count++].text = strdup(ev->text ? ev->text : "");
    }
    mtx_unlock(&r->lock);
}

static double number_arg(ui_js *js, const ui_js_call_info *call, uint32_t i) {
    double d = 0;
    if (i < call->argc) ui_js_to_number(js, call->args[i], &d);
    return d;
}

static ui_js_val t_request_step(ui_js *js, const ui_js_call_info *call) {
    runner *r = call->data;
    double ms = number_arg(js, call, 0);
    atomic_fetch_add(&r->step_ns, (unsigned long long)(ms > 0 ? ms * 1e6 + 0.5 : 0));
    return ui_js_undefined(js);
}

static ui_js_val t_done(ui_js *js, const ui_js_call_info *call) {
    runner *r = call->data;
    r->failed = (int)number_arg(js, call, 0);
    r->total = (int)number_arg(js, call, 1);
    atomic_store(&r->done, true);
    return ui_js_undefined(js);
}

static ui_js_val t_hooks(ui_js *js, const ui_js_call_info *call) {
    runner *r = call->data;
    ui_js_val list = ui_js_array(js);
    mtx_lock(&r->lock);
    for (uint32_t i = 0; i < r->hook_count; i++) {
        ui_js_val h = ui_js_object(js);
        ui_js_set(js, h, "kind", ui_js_number(js, r->hooks[i].kind - UI_TRACE_SCRIPT_EXCEPTION));
        ui_js_set(js, h, "text", ui_js_string(js, r->hooks[i].text, strlen(r->hooks[i].text)));
        ui_js_set_index(js, list, i, h);
    }
    mtx_unlock(&r->lock);
    return list;
}

static ui_js_val t_gc(ui_js *js, const ui_js_call_info *call) {
    (void)call;
    ui_js_gc(js);
    return ui_js_undefined(js);
}

static ui_js_val t_c_buffer(ui_js *js, const ui_js_call_info *call) {
    char *name = call->argc ? ui_js_to_utf8(js, call->args[0], NULL) : NULL;
    if (!name) return (ui_js_val){0};
    uint8_t *bytes;
    size_t size;
    bool found = ui_test_reference_commands(name, &bytes, &size);
    free(name);
    if (!found) return ui_js_throw_error(js, UI_JS_ERROR, "cBuffer: no such case");
    ui_js_val ab = ui_js_array_buffer_copy(js, bytes, size);
    free(bytes);
    return ab;
}

static ui_js_val t_c_events(ui_js *js, const ui_js_call_info *call) {
    (void)call;
    size_t size;
    uint8_t *bytes = ui_test_reference_events(&size);
    ui_js_val ab = ui_js_array_buffer_copy(js, bytes, size);
    free(bytes);
    return ab;
}

static ui_js_val t_heap_used(ui_js *js, const ui_js_call_info *call) {
    (void)call;
    ui_js_memory m;
    ui_js_memory_stats(js, &m);
    return ui_js_number(js, (double)m.used_bytes);
}

static ui_js_val t_heap_detail(ui_js *js, const ui_js_call_info *call) {
    (void)call;
    ui_js_memory m;
    ui_js_memory_stats(js, &m);
    ui_js_val o = ui_js_object(js);
    ui_js_set(js, o, "objects", ui_js_number(js, (double)m.objects));
    ui_js_set(js, o, "functions", ui_js_number(js, (double)m.functions));
    ui_js_set(js, o, "bytecodeBytes", ui_js_number(js, (double)m.bytecode_bytes));
    ui_js_set(js, o, "stringBytes", ui_js_number(js, (double)m.string_bytes));
    return o;
}

static ui_js_val t_read_text(ui_js *js, const ui_js_call_info *call) {
    char *name = call->argc ? ui_js_to_utf8(js, call->args[0], NULL) : NULL;
    char path[8192];
    snprintf(path, sizeof path, "%s/%s", AXE_SOURCE_DIR, name ? name : "");
    free(name);
    FILE *f = fopen(path, "rb");
    if (!f) return ui_js_throw_error(js, UI_JS_TYPE_ERROR, "can't read a file");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)n);
    size_t got = text ? fread(text, 1, (size_t)n, f) : 0;
    fclose(f);
    ui_js_val v = ui_js_string(js, text ? text : "", got);
    free(text);
    return v;
}

static ui_js_val t_capture(ui_js *js, const ui_js_call_info *call) {
    runner *r = call->data;
    char *path = call->argc ? ui_js_to_utf8(js, call->args[0], NULL) : NULL;
    free(atomic_exchange(&r->capture, path));
    return ui_js_undefined(js);
}

#ifdef AXE_DEV
static void make_parents(char *path) {
    for (char *p = strchr(path + 1, '/'); p; p = strchr(p + 1, '/')) {
        *p = 0;
        mkdir(path, 0777);
        *p = '/';
    }
}

// hotEdit({path: text | null}): the same build axe-run's watcher runs.
static ui_js_val t_hot_edit(ui_js *js, const ui_js_call_info *call) {
    runner *r = call->data;
    ui_js_val keys_fn, keys, global = ui_js_global(js), object;
    uint32_t n = 0;
    if (!ui_js_get(js, global, "Object", &object) || !ui_js_get(js, object, "keys", &keys_fn) ||
        !ui_js_call(js, keys_fn, object, 1, call->args, &keys) || !ui_js_length(js, keys, &n))
        return (ui_js_val){0};
    const char **files = calloc(n ? n : 1, sizeof *files);
    for (uint32_t i = 0; i < n; i++) {
        ui_js_val key, value;
        if (!ui_js_get_index(js, keys, i, &key)) return (ui_js_val){0};
        char *name = ui_js_to_utf8(js, key, NULL);
        if (!name || !ui_js_get(js, call->args[0], name, &value)) return (ui_js_val){0};
        files[i] = name;
        size_t size = strlen(r->build.root) + strlen(name) + 2;
        char *path = malloc(size);
        snprintf(path, size, "%s/%s", r->build.root, name);
        make_parents(path);
        if (ui_js_typeof(js, value) == UI_JS_NULL) {
            unlink(path);
        } else {
            char *text = ui_js_to_utf8(js, value, NULL);
            FILE *f = fopen(path, "wb");
            if (f && text) fputs(text, f);
            if (f) fclose(f);
            free(text);
        }
        free(path);
    }
    ui_dev_batch *b = ui_dev_build(&r->build, files, n, ui_monotonic_ns());
    if (b) ui_dev_channel_post(r->channel, b);
    for (uint32_t i = 0; i < n; i++) free((void *)(uintptr_t)files[i]);
    free(files);
    return ui_js_undefined(js);
}
#endif

#ifdef AXE_DEV
static ui_js_val t_hot_save(ui_js *js, const ui_js_call_info *call) {
    runner *r = call->data;
    char *name = ui_js_to_utf8(js, call->args[0], NULL);
    char *text = ui_js_to_utf8(js, call->args[1], NULL);
    char *how = ui_js_to_utf8(js, call->args[2], NULL);
    if (!name || !text || !how) return (ui_js_val){0};
    char path[8192], temp[8192 + 8];
    snprintf(path, sizeof path, "%s/%s", r->build.root, name);
    snprintf(temp, sizeof temp, "%s.tmp~", path);
    make_parents(path);
    if (!strcmp(how, "delete")) {
        unlink(path);
    } else {
        if (!strcmp(how, "recreate")) unlink(path);
        bool rename_over = !strcmp(how, "rename");
        FILE *f = fopen(rename_over ? temp : path, "wb");
        if (f) {
            fputs(text, f);
            fclose(f);
        }
        if (rename_over) rename(temp, path);
    }
    free(name);
    free(text);
    free(how);
    return ui_js_undefined(js);
}
#endif

static void setup(void *user, ui_js *js) {
    ui_js_val m = ui_js_object(js);
    ui_js_set(js, m, "requestStep", ui_js_function(js, "requestStep", 1, t_request_step, user));
    ui_js_set(js, m, "done", ui_js_function(js, "done", 2, t_done, user));
    ui_js_set(js, m, "hooks", ui_js_function(js, "hooks", 0, t_hooks, user));
    ui_js_set(js, m, "gc", ui_js_function(js, "gc", 0, t_gc, user));
    ui_js_set(js, m, "cBuffer", ui_js_function(js, "cBuffer", 1, t_c_buffer, user));
    ui_js_set(js, m, "cEvents", ui_js_function(js, "cEvents", 0, t_c_events, user));
    ui_js_set(js, m, "capture", ui_js_function(js, "capture", 1, t_capture, user));
    ui_js_set(js, m, "readText", ui_js_function(js, "readText", 1, t_read_text, user));
    ui_js_set(js, m, "heapUsed", ui_js_function(js, "heapUsed", 0, t_heap_used, user));
    ui_js_set(js, m, "heapDetail", ui_js_function(js, "heapDetail", 0, t_heap_detail, user));
#ifdef AXE_DEV
    ui_js_set(js, m, "hotEdit", ui_js_function(js, "hotEdit", 1, t_hot_edit, user));
    ui_js_set(js, m, "hotSave", ui_js_function(js, "hotSave", 3, t_hot_save, user));
#endif
    ui_js_define_module(js, "axe:test", m);
}

#ifdef AXE_DEV
static void wake_core(void *user) { ui_app_request_turn(user); }
#endif

int main(int argc, char **argv) {
    const char *bundle = NULL;
    const char *args[8];
    uint32_t arg_count = 0;
    ui_scheduler_kind scheduler = UI_SCHEDULER_FROM_ENV;
#ifdef AXE_DEV
    const char *dev_dir = NULL, *entry = "main.tsx";
    bool watch = false;
#endif
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scheduler") && i + 1 < argc) {
            scheduler = !strcmp(argv[++i], "single") ? UI_SCHEDULER_SINGLE : UI_SCHEDULER_THREADED;
        } else if (!strcmp(argv[i], "--arg") && i + 1 < argc && arg_count < 8) {
            args[arg_count++] = argv[++i];
#ifdef AXE_DEV
        } else if (!strcmp(argv[i], "--dev") && i + 1 < argc) {
            dev_dir = argv[++i];
        } else if (!strcmp(argv[i], "--entry") && i + 1 < argc) {
            entry = argv[++i];
        } else if (!strcmp(argv[i], "--watch")) {
            watch = true;
#endif
        } else {
            bundle = argv[i];
        }
    }
    if (!bundle) {
        fprintf(stderr, "usage: %s bundle.js [--scheduler single|threaded]\n", argv[0]);
        return 2;
    }
    runner r = {0};
    mtx_init(&r.lock, mtx_plain);
    ui_script_internal internal = {.setup = setup, .user = &r, .performance_from_animation_clock = true};
    // A runaway test fails on JS's out-of-memory error instead of taking the machine's memory.
    ui_script_desc script = {.bundle_path = bundle, .internal = &internal, .memory_limit = (size_t)512 << 20,
                             .args = args, .arg_count = arg_count};
#ifdef AXE_DEV
    char src_dir[4096], work_dir[4096], command[8300];
    if (dev_dir) {
        snprintf(src_dir, sizeof src_dir, "%s/src", dev_dir);
        snprintf(work_dir, sizeof work_dir, "%s/work", dev_dir);
        snprintf(command, sizeof command, "rm -rf '%s' '%s' && mkdir -p '%s' '%s'", src_dir, work_dir, src_dir, work_dir);
        if (system(command) != 0) return 1;
        r.build = (ui_dev_build_desc){src_dir, work_dir, AXE_TSX_PATH, AXE_ESBUILD_PATH, AXE_SOURCE_DIR};
        ui_dev_desc d = {.entry = entry, .packages = AXE_DEV_PACKAGES, .driver = bundle};
        if (ui_dev_channel_create(&d, &r.channel) != UI_OK) return 1;
        internal.dev = r.channel;
        script.bundle_path = NULL;
    }
#endif
    static const char *const fonts[] = {AXE_SOURCE_DIR "/tests/fonts/NotoSans-VF.ttf"};
    ui_app_desc desc = {
        .scheduler = scheduler,
        .surface = {.kind = UI_SURFACE_HEADLESS, .width = 320, .height = 240},
        .script = &script,
        .start_paused = true,
        .text = {.font_files = fonts, .font_file_count = 1},
    };
    ui_app *app;
    if (ui_app_create(&desc, &app) != UI_OK) return 1;
    ui_hooks_subscribe(app, on_trace, &r, NULL);
#ifdef AXE_DEV
    if (r.channel) ui_dev_channel_set_wake(r.channel, wake_core, app);
    ui_dev_session *session = watch && r.channel ? ui_dev_session_start(&r.build, r.channel) : NULL;
#endif
    int frames = 0;
    ui_result res = UI_OK;
    while (res == UI_OK && !atomic_load(&r.done) && frames++ < MAX_FRAMES) {
        uint64_t step = atomic_exchange(&r.step_ns, 0);
        if (step) {
            // What the test wrote before stepping applies at the current time first:
            // `await step(ms)` lets ms pass after it.
            res = ui_app_frame(app);
            ui_app_clock_step(app, step);
        }
        if (res == UI_OK) res = ui_app_frame(app);
#ifdef AXE_DEV
        // The watcher works in wall time: the frame budget has to mean something to it.
        if (session) thrd_sleep(&(struct timespec){.tv_nsec = 1000000}, NULL);
#endif
        char *capture = atomic_exchange(&r.capture, NULL);
        if (capture) {
            ui_image image;
            if (res == UI_OK) res = ui_app_frame(app);
            if (res == UI_OK) res = ui_surface_capture(app, ui_app_main_surface(app), &image);
            if (res == UI_OK) {
                res = ui_image_write_ppm(&image, capture);
                ui_image_free(&image);
            }
            free(capture);
        }
    }
#ifdef AXE_DEV
    ui_dev_session_stop(session);
#endif
    ui_app_destroy(app);
#ifdef AXE_DEV
    ui_dev_channel_destroy(r.channel);
#endif
    for (uint32_t i = 0; i < r.hook_count; i++) free(r.hooks[i].text);
    if (!atomic_load(&r.done)) {
        fprintf(stderr, "%s: the tests didn't finish within %d frames\n", bundle, MAX_FRAMES);
        return 1;
    }
    printf("%s: %d of %d tests passed\n", bundle, r.total - r.failed, r.total);
    return r.failed || !r.total ? 1 : 0;
}
