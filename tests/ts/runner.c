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
//   capture(path)    writes the surface as a PPM after the next frame
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
#endif
#include "script/host/host.h"
#include "ui/ui.h"
#include "wire/gen/wire_gen.h"

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

// ---- The encoder's reference cases ----------------------------------------------------------
// tests/ts/wire.test.ts encodes the same ops with @axe/wire and @axe/core and compares bytes.

static ui_handle node(uint32_t slot, uint32_t gen) { return (ui_handle){slot, gen}; }
static const ui_handle surface = {(uint32_t)UI_HANDLE_SURFACE << UI_HANDLE_SLOT_BITS, 1};

static void encode_case(ui_cmd_writer *w, const char *name) {
    ui_handle a = node(1, 1), b = node(2, 3);
    if (!strcmp(name, "basic")) {
        ui_cmd_create_node(w, a, surface);
        ui_cmd_create_node(w, b, a);
        ui_cmd_set_rect(w, a, 1.5f, -2, 300, 40.25f);
        ui_cmd_set_corner_radius(w, a, 8);
        ui_cmd_set_color(w, a, 0.1f, 0.2f, 0.3f, 1);
        ui_cmd_set_clip(w, a, true);
        ui_cmd_move_node(w, b, surface, (ui_handle){0});
        ui_cmd_move_node(w, b, a, a);
        ui_cmd_set_state(w, b, 3, 1);
        ui_cmd_request_layout(w, b, 77);
        ui_cmd_set_timer(w, 9, 123456789012345ull);
        ui_cmd_cancel_timer(w, 9);
        ui_cmd_load_font(w, 4, "/fonts/Noto \xC3\xA9.ttf");
        ui_cmd_destroy_style(w, 5);
        ui_cmd_set_styles(w, b, (uint32_t[]){3, 1, 2}, 3);
        ui_cmd_set_styles(w, b, NULL, 0);
        ui_cmd_start_spring(w, a, UI_PROP_RECT, (float[]){1, 2, 3, 4}, 170, 18.25f);
        ui_cmd_start_spring(w, a, UI_PROP_CORNER_RADIUS, (float[]){6}, 90, 19);
        ui_cmd_destroy_node(w, b);
    } else if (!strcmp(name, "styles")) {
        ui_style_decl base[] = {
            {UI_STYLE_WIDTH, ui_px(208)},
            {UI_STYLE_HEIGHT, ui_percent(50)},
            {UI_STYLE_FLEX_GROW, ui_number(1)},
            {UI_STYLE_MARGIN_LEFT, ui_auto()},
            {UI_STYLE_MAX_WIDTH, ui_none()},
            {UI_STYLE_BACKGROUND_COLOR, ui_rgba(0.2f, 0.45f, 0.95f, 1)},
            {UI_STYLE_FLEX_DIRECTION, ui_keyword(UI_FLEX_COLUMN)},
            {UI_STYLE_TEXT_COLOR, ui_token_ref(3)},
            {UI_STYLE_FONT_FAMILY, ui_string("Noto Sans, sans-serif")},
            {UI_STYLE_LANGUAGE, ui_string("sr-Cyrl")},
            {UI_STYLE_DIRECTION, ui_inherit()},
            {UI_STYLE_OPACITY, ui_initial()},
            {UI_STYLE_TRANSITION(UI_STYLE_BACKGROUND_COLOR), ui_transition(90, 19)},
            {UI_STYLE_TRANSITION(UI_STYLE_LAYOUT), ui_transition(170, 24)},
        };
        ui_style_decl hover[] = {{UI_STYLE_BACKGROUND_COLOR, ui_token_ref(7)}};
        ui_style_block_desc blocks[] = {{.decls = base, .count = UI_ARRAY_LEN(base)}, {.states = UI_STATE_HOVER | UI_STATE_PRESSED, .decls = hover, .count = 1}};
        ui_cmd_define_style(w, 1, blocks, 2);
        ui_cmd_define_style(w, 2, NULL, 0);
        ui_style_decl overrides[] = {{UI_STYLE_BORDER_WIDTH, ui_px(2)}, {UI_STYLE_WIDTH, ui_unset()}};
        ui_cmd_set_props(w, a, overrides, 2);
        ui_cmd_set_token(w, 3, ui_rgba(1, 1, 1, 1));
        ui_cmd_set_token(w, 4, ui_token_ref(3));
        ui_cmd_set_token(w, 5, ui_string("x"));
        ui_cmd_set_token(w, 3, ui_unset());
    } else if (!strcmp(name, "scopes")) {
        // Token declarations: a block with some (12-byte header), one without (8), and a node's.
        ui_style_decl base[] = {{UI_STYLE_BACKGROUND_COLOR, ui_token_ref(3)}};
        ui_token_decl scope[] = {{3, ui_rgba(0.25f, 0.5f, 1, 1)}, {4, ui_token_ref(5)}, {6, ui_string("Noto Serif")}};
        ui_token_decl hover[] = {{3, ui_px(2)}};
        ui_style_block_desc blocks[] = {{.decls = base, .count = 1, .tokens = scope, .token_count = 3},
                                        {.states = UI_STATE_HOVER, .tokens = hover, .token_count = 1},
                                        {.states = UI_STATE_PRESSED, .decls = base, .count = 1}};
        ui_cmd_define_style(w, 1, blocks, 3);
        ui_token_decl own[] = {{3, ui_rgba(1, 0, 0, 1)}, {9, ui_unset()}, {10, ui_keyword(2)}};
        ui_cmd_set_token_overrides(w, a, own, 3);
        ui_cmd_set_token_overrides(w, a, NULL, 0);
    } else if (!strcmp(name, "text")) {
        // As @axe/core's setText writes it: one family list, a language, spans, an ellipsis.
        static const char *const families[] = {"Noto Sans, sans-serif", "Noto Serif"};
        static const char *const languages[] = {"ro"};
        const char *s = "h\xC3\xA9llo \xF0\x9F\x98\x80 world";
        ui_text_span_desc spans[] = {
            {.start = 1, .end = 3, .style = {.weight = 700}, .set = UI_TEXT_SET_WEIGHT},
            {.start = 7, .end = 11, .style = {.family = 1, .language = 1, .size = 20},
             .set = UI_TEXT_SET_FAMILY | UI_TEXT_SET_LANGUAGE | UI_TEXT_SET_SIZE},
        };
        ui_text_desc t = {
            .text = s, .length = (uint32_t)strlen(s),
            .style = {.family = 0, .size = 16, .weight = 400, .color = {0, 0, 0, 1}},
            .spans = spans, .span_count = 2, .families = families, .family_count = 2,
            .languages = languages, .language_count = 1, .max_lines = 2, .ellipsis = "...",
            .align = UI_TEXT_ALIGN_CENTER, .line_height = 1.4f,
            .set = UI_TEXT_SET_FAMILY | UI_TEXT_SET_SIZE | UI_TEXT_SET_WEIGHT | UI_TEXT_SET_ALIGN |
                   UI_TEXT_SET_LINE_HEIGHT,
        };
        ui_cmd_set_text(w, a, &t);
        ui_text_desc plain = {.text = "Open", .length = 4};
        ui_cmd_set_text(w, b, &plain);
    } else if (!strcmp(name, "long_text")) {
        // Longer than one op: set_text and append_text ops.
        static char big[200000];
        for (size_t i = 0; i < sizeof big; i++) big[i] = (char)('a' + i % 26);
        ui_cmd_set_text(w, a, &(ui_text_desc){.text = big, .length = sizeof big, .ellipsis = ""});
    }
}

static ui_js_val t_c_buffer(ui_js *js, const ui_js_call_info *call) {
    char *name = call->argc ? ui_js_to_utf8(js, call->args[0], NULL) : NULL;
    if (!name) return (ui_js_val){0};
    ui_cmd_writer *w;
    ui_cmd_writer_create(&w);
    encode_case(w, name);
    free(name);
    uint8_t *bytes;
    size_t size;
    if (ui_cmd_writer_take(w, &bytes, &size) != UI_OK) {
        ui_cmd_writer_destroy(w);
        return ui_js_throw_error(js, UI_JS_ERROR, "cBuffer: no such case");
    }
    ui_cmd_writer_destroy(w);
    ui_js_val ab = ui_js_array_buffer_copy(js, bytes, size);
    free(bytes);
    return ab;
}

static ui_js_val t_c_events(ui_js *js, const ui_js_call_info *call) {
    (void)call;
    ui_wire_buf b;
    ui_wire_buf_init(&b, UI_WIRE_MAGIC_EVENT);
    ui_wire_clock_put(&b, &(ui_wire_clock){(1ull << 60) + 12345});
    ui_wire_frame_committed_put(&b, &(ui_wire_frame_committed){surface, 42, 16666667});
    ui_wire_surface_resized_put(&b, &(ui_wire_surface_resized){surface, 980, 640.5f, 150});
    ui_wire_surface_close_requested_put(&b, &(ui_wire_surface_close_requested){surface});
    ui_wire_timer_fired_put(&b, &(ui_wire_timer_fired){7, 2500000000ull, 2516666667ull});
    ui_wire_font_loaded_put(&b, &(ui_wire_font_loaded){3, UI_ERR_IO});
    ui_wire_layout_measured m = {.request_id = 9, .result = UI_OK, .node = node(5, 2), .surface = surface};
    ui_wire_rect *rects[4] = {&m.layout_in_parent, &m.layout_in_surface, &m.presentation_in_parent,
                              &m.presentation_in_surface};
    for (int i = 0; i < 4; i++) *rects[i] = (ui_wire_rect){(float)i, (float)i + 0.5f, 100.25f, 20};
    ui_wire_layout_measured_put(&b, &m);
    uint8_t *future = ui_wire_buf_op(&b, 999, 8); // from a newer core: skipped
    memset(future, 0xAB, 8);
    size_t size;
    uint8_t *bytes = ui_wire_buf_finish(&b, &size);
    ui_js_val ab = ui_js_array_buffer_copy(js, bytes, size);
    free(bytes);
    return ab;
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

static void setup(void *user, ui_js *js) {
    ui_js_val m = ui_js_object(js);
    ui_js_set(js, m, "requestStep", ui_js_function(js, "requestStep", 1, t_request_step, user));
    ui_js_set(js, m, "done", ui_js_function(js, "done", 2, t_done, user));
    ui_js_set(js, m, "hooks", ui_js_function(js, "hooks", 0, t_hooks, user));
    ui_js_set(js, m, "gc", ui_js_function(js, "gc", 0, t_gc, user));
    ui_js_set(js, m, "cBuffer", ui_js_function(js, "cBuffer", 1, t_c_buffer, user));
    ui_js_set(js, m, "cEvents", ui_js_function(js, "cEvents", 0, t_c_events, user));
    ui_js_set(js, m, "capture", ui_js_function(js, "capture", 1, t_capture, user));
#ifdef AXE_DEV
    ui_js_set(js, m, "hotEdit", ui_js_function(js, "hotEdit", 1, t_hot_edit, user));
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
