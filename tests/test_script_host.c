// The script host (src/script/host/, docs/script.md) with raw JS bundles in a headless app:
// the turn's order (the bundle, then events, each followed by a microtask checkpoint),
// timers on a stepped animation clock (identical under both schedulers), reporting of
// uncaught exceptions and unhandled rejections through the hooks while the app keeps
// running, long tasks, leak reports, commands submitted from JS, the text globals, and
// bytecode bundles.
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "runtime/common.h"
#include "script/host/host.h"
#include "test.h"
#include "ui/ui.h"

#define MS 1000000ull

typedef struct capture {
    mtx_t lock;
    char lines[256][160];
    uint32_t count;
    char hooks[64][512];
    ui_trace_kind kinds[64];
    uint64_t durations[64];
    uint32_t hook_count;
} capture;

static void add_line(capture *c, const char *s) {
    mtx_lock(&c->lock);
    if (c->count < 256) snprintf(c->lines[c->count++], sizeof c->lines[0], "%s", s);
    mtx_unlock(&c->lock);
}

static ui_js_val native_record(ui_js *js, const ui_js_call_info *call) {
    char *s = call->argc ? ui_js_to_utf8(js, call->args[0], NULL) : NULL;
    if (!s) return (ui_js_val){0};
    add_line(call->data, s);
    free(s);
    return ui_js_undefined(js);
}

static ui_js_val native_wall(ui_js *js, const ui_js_call_info *call) {
    (void)call;
    return ui_js_number(js, (double)ui_monotonic_ns() / 1e6);
}

static void setup(void *user, ui_js *js) {
    ui_js_val exports = ui_js_object(js);
    ui_js_set(js, exports, "record", ui_js_function(js, "record", 1, native_record, user));
    ui_js_set(js, exports, "wall", ui_js_function(js, "wall", 0, native_wall, user));
    CHECK(ui_js_define_module(js, "axe:test", exports));
}

static void on_trace(void *user, const ui_trace_event *ev) {
    if (ev->kind < UI_TRACE_SCRIPT_EXCEPTION) return;
    capture *c = user;
    mtx_lock(&c->lock);
    if (c->hook_count < 64) {
        c->kinds[c->hook_count] = ev->kind;
        c->durations[c->hook_count] = ev->duration_ns;
        snprintf(c->hooks[c->hook_count++], sizeof c->hooks[0], "%s", ev->text ? ev->text : "");
    }
    mtx_unlock(&c->lock);
}

typedef struct run_desc {
    const char *src;
    const void *bundle; // instead of src
    size_t bundle_size;
    ui_scheduler_kind scheduler;
    int frames;         // after the first, each 10 ms of the paused clock later
    uint64_t long_task_ns;
} run_desc;

static void run(const run_desc *rd, capture *c) {
    memset(c, 0, sizeof *c);
    mtx_init(&c->lock, mtx_plain);
    ui_script_internal internal = {.setup = setup, .user = c};
    ui_script_desc script = {
        .bundle = rd->bundle ? rd->bundle : rd->src,
        .bundle_size = rd->bundle ? rd->bundle_size : strlen(rd->src),
        .bundle_name = "test.js",
        .long_task_ns = rd->long_task_ns,
        .internal = &internal,
    };
    ui_app_desc desc = {
        .scheduler = rd->scheduler,
        .surface = {.kind = UI_SURFACE_HEADLESS, .width = 64, .height = 64},
        .script = &script,
        .start_paused = true,
    };
    ui_app *app;
    CHECK(ui_app_create(&desc, &app) == UI_OK);
    if (!app) return;
    CHECK(ui_hooks_subscribe(app, on_trace, c, NULL) == UI_OK);
    CHECK(ui_app_frame(app) == UI_OK);
    for (int i = 0; i < rd->frames; i++) {
        ui_app_clock_step(app, 10 * MS);
        CHECK(ui_app_frame(app) == UI_OK);
    }
    ui_app_stats stats;
    ui_app_get_stats(app, &stats);
    if (stats.commands_dropped) fprintf(stderr, "  %llu commands dropped\n", (unsigned long long)stats.commands_dropped);
    ui_app_destroy(app);
}

static bool has_line(const capture *c, const char *s) {
    for (uint32_t i = 0; i < c->count; i++)
        if (!strcmp(c->lines[i], s)) return true;
    return false;
}

static int index_of(const capture *c, const char *s) {
    for (uint32_t i = 0; i < c->count; i++)
        if (!strcmp(c->lines[i], s)) return (int)i;
    return -1;
}

static int hook_with(const capture *c, ui_trace_kind kind, const char *needle) {
    for (uint32_t i = 0; i < c->hook_count; i++)
        if (c->kinds[i] == kind && strstr(c->hooks[i], needle)) return (int)i;
    return -1;
}

static void dump(const capture *c) {
    for (uint32_t i = 0; i < c->count; i++) fprintf(stderr, "    %s\n", c->lines[i]);
    for (uint32_t i = 0; i < c->hook_count; i++) fprintf(stderr, "    hook %d: %s\n", c->kinds[i], c->hooks[i]);
}

// The bundle runs before the first turn's events; a checkpoint follows each event.
static void test_turn_order(void) {
    const char *src = "import { setDispatcher, animationTime } from 'axe:host';\n"
                      "import { record } from 'axe:test';\n"
                      "record(`eval at ${animationTime()}`);\n"
                      "Promise.resolve().then(() => record('eval then'));\n"
                      "setDispatcher((events, type, at, size) => {\n"
                      "  const u32 = new Uint32Array(events, at, size >> 2);\n"
                      "  record(`event ${type} ${size}`);\n"
                      "  Promise.resolve().then(() => record(`then ${type}`));\n"
                      "});\n";
    capture c;
    run(&(run_desc){.src = src, .frames = 0}, &c);
    CHECK(c.count >= 4);
    CHECK(index_of(&c, "eval at 0") == 0);
    CHECK(index_of(&c, "eval then") == 1);
    CHECK(index_of(&c, "event 7 8") == 2);  // the clock: the turn's start
    CHECK(index_of(&c, "then 7") == 3);     // before the next event's handler
    CHECK(index_of(&c, "event 2 20") == 4); // surface_resized
    CHECK(index_of(&c, "then 2") == 5);
    CHECK(c.hook_count == 0);
    if (test_failures) dump(&c);
}

static const char *const timers_src =
    "import { animationTime } from 'axe:host';\n"
    "import { record } from 'axe:test';\n"
    "const at = (s) => record(`${s} @${animationTime() / 1e6}`);\n"
    "setTimeout(() => at('t30'), 30);\n"
    "setTimeout(() => at('t10'), 10);\n"
    "setTimeout((a, b) => at(`t10b ${a}${b}`), 10, 'x', 'y');\n"
    "let n = 0;\n"
    "const iv = setInterval(() => { at(`iv${n}`); if (++n === 3) clearInterval(iv); }, 20);\n"
    "setTimeout(() => { at('zero'); setTimeout(() => at('nested0'), 0); }, 0);\n"
    "const c = setTimeout(() => at('cancelled'), 15); clearTimeout(c);\n"
    "setTimeout(() => at('neg'), -5);\n"
    "record(`ids ${typeof iv} ${iv > 0}`);\n";

static void timer_order(ui_scheduler_kind scheduler, capture *c) {
    run(&(run_desc){.src = timers_src, .scheduler = scheduler, .frames = 10}, c);
    CHECK(has_line(c, "ids number true"));
    CHECK(!has_line(c, "cancelled @15"));
    // Commands apply at the next frame, 10 ms later: a timer set at 0 for 0 fires at 10.
    int zero = index_of(c, "zero @10"), neg = index_of(c, "neg @10"), nested = -1;
    for (uint32_t i = 0; i < c->count; i++)
        if (!strncmp(c->lines[i], "nested0 @", 9)) nested = (int)i;
    CHECK(zero >= 0 && neg == zero + 1); // same deadline: creation order
    CHECK(nested > neg);                 // set during a timer: never in the same batch
    CHECK(index_of(c, "t10 @10") >= 0 && index_of(c, "t10b xy @10") == index_of(c, "t10 @10") + 1);
    // An interval follows its deadlines (20, 40, 60) and stops when cleared.
    CHECK(index_of(c, "iv0 @20") >= 0 && index_of(c, "iv1 @40") >= 0 && index_of(c, "iv2 @60") >= 0);
    CHECK(index_of(c, "t30 @30") > index_of(c, "iv0 @20") && index_of(c, "t30 @30") < index_of(c, "iv1 @40"));
    for (uint32_t i = 0; i < c->count; i++) CHECK(strncmp(c->lines[i], "iv3", 3) != 0);
}

static void test_timers_deterministic(void) {
    capture a, b;
    timer_order(UI_SCHEDULER_SINGLE, &a);
    timer_order(UI_SCHEDULER_THREADED, &b);
    CHECK(a.count == b.count);
    for (uint32_t i = 0; i < a.count && i < b.count; i++) CHECK(!strcmp(a.lines[i], b.lines[i]));
    if (test_failures) dump(&a), dump(&b);
}

// Paused, the clock only moves when stepped: no timer fires without a step.
static void test_timers_paused(void) {
    const char *src = "import { record } from 'axe:test';\n"
                      "setTimeout(() => record('fired'), 5);\n";
    capture c;
    run(&(run_desc){.src = src, .frames = 0}, &c);
    CHECK(!has_line(&c, "fired"));
    run(&(run_desc){.src = src, .frames = 2}, &c);
    CHECK(has_line(&c, "fired"));
}

static void test_errors_reported(void) {
    const char *src =
        "import { setDispatcher } from 'axe:host';\n"
        "import { record } from 'axe:test';\n"
        "let dispatched = 0;\n"
        "setDispatcher(function onEvent() { if (!dispatched++) throw new TypeError('in dispatch'); });\n"
        "Promise.reject(new Error('nobody'));\n"
        "const handledLater = Promise.reject(new Error('handled'));\n"
        "handledLater.catch(() => record('caught'));\n"
        "setTimeout(function inTimer() { throw new RangeError('in timer'); }, 0);\n"
        "queueMicrotask(() => { throw new Error('in microtask'); });\n"
        "setTimeout(() => record('alive'), 20);\n";
    capture c;
    run(&(run_desc){.src = src, .frames = 3}, &c);
    CHECK(hook_with(&c, UI_TRACE_SCRIPT_EXCEPTION, "Uncaught TypeError: in dispatch") >= 0);
    CHECK(hook_with(&c, UI_TRACE_SCRIPT_EXCEPTION, "onEvent") >= 0); // the stack trace
    CHECK(hook_with(&c, UI_TRACE_SCRIPT_EXCEPTION, "Uncaught RangeError: in timer (in a setTimeout callback)") >= 0);
    CHECK(hook_with(&c, UI_TRACE_SCRIPT_EXCEPTION, "inTimer") >= 0);
    CHECK(hook_with(&c, UI_TRACE_SCRIPT_EXCEPTION, "Uncaught Error: in microtask") >= 0);
    CHECK(hook_with(&c, UI_TRACE_SCRIPT_REJECTION, "Uncaught (in promise) Error: nobody") >= 0);
    CHECK(hook_with(&c, UI_TRACE_SCRIPT_REJECTION, "Error: handled") < 0); // handled in the same checkpoint
    CHECK(has_line(&c, "caught") && has_line(&c, "alive"));         // and the app keeps running
    if (test_failures) dump(&c);
}

static void test_load_errors(void) {
    capture c;
    run(&(run_desc){.src = "let = = 1;\n", .frames = 1}, &c);
    CHECK(hook_with(&c, UI_TRACE_SCRIPT_EXCEPTION, "SyntaxError") >= 0);
    CHECK(hook_with(&c, UI_TRACE_SCRIPT_EXCEPTION, "loading the app") >= 0);
    run(&(run_desc){.src = "import { record } from 'axe:test';\nrecord('ran');\nthrow new Error('at load');\n",
                    .frames = 1},
        &c);
    CHECK(has_line(&c, "ran"));
    CHECK(hook_with(&c, UI_TRACE_SCRIPT_EXCEPTION, "Uncaught Error: at load (evaluating the app)") >= 0);
    run(&(run_desc){.src = "import './elsewhere.js';\n", .frames = 1}, &c);
    CHECK(hook_with(&c, UI_TRACE_SCRIPT_EXCEPTION, "elsewhere.js") >= 0);
    if (test_failures) dump(&c);
}

static void test_long_task(void) {
    const char *src = "import { setDispatcher } from 'axe:host';\n"
                      "import { wall } from 'axe:test';\n"
                      "setDispatcher(() => { const end = wall() + 20; while (wall() < end); });\n";
    capture c;
    run(&(run_desc){.src = src, .frames = 0, .long_task_ns = 5 * MS}, &c);
    int i = hook_with(&c, UI_TRACE_SCRIPT_LONG_TASK, "event surface_resized");
    CHECK(i >= 0 && c.durations[i] >= 20 * MS);
}

static void test_leak_report(void) {
    const char *src = "import { reportLeak, debug } from 'axe:host';\n"
                      "import { record } from 'axe:test';\n"
                      "record(`debug ${typeof debug}`);\n"
                      "reportLeak(3, 2, 4);\n";
    capture c;
    run(&(run_desc){.src = src}, &c);
    CHECK(has_line(&c, "debug boolean"));
    CHECK(hook_with(&c, UI_TRACE_SCRIPT_LEAK, "00000003:2 and its subtree") >= 0);
}

// JS writes a buffer into the host's storage and submits it; the core applies it.
static void test_commands(void) {
    const char *src =
        "import { setDispatcher, setFlush, commandBuffer, growCommandBuffer, submitCommands } from 'axe:host';\n"
        "import { record } from 'axe:test';\n"
        "let surface = null, size = 16, ops = 0, buf = commandBuffer();\n"
        "const op = (words) => {\n"
        "  if (size + words.length * 4 > buf.byteLength) buf = growCommandBuffer(size + words.length * 4);\n"
        "  new Uint32Array(buf, size, words.length).set(words); size += words.length * 4; ops++;\n"
        "};\n"
        "setDispatcher((events, type, at) => {\n"
        "  if (type !== 2 || surface) return;\n" // surface_resized: create a node on it
        "  const u = new Uint32Array(events, at, 2); surface = [u[0], u[1]];\n"
        "  op([1 | (20 << 16), 1, 1, surface[0], surface[1]]);\n" // create_node {1,1}
        "  for (let i = 0; i < 4000; i++) op([17 | (20 << 16), 1, 1, 1, 1]);\n" // set_state, to grow
        "});\n"
        "setFlush(() => {\n"
        "  if (!ops) return;\n"
        "  new Uint32Array(buf, 0, 4).set([0x42434955, 0, size, ops]);\n"
        "  buf = submitCommands(size); size = 16; ops = 0;\n"
        "  record(`submitted ${buf.byteLength}`);\n"
        "});\n";
    capture c;
    run(&(run_desc){.src = src, .frames = 1}, &c);
    CHECK(has_line(&c, "submitted 65536"));
    CHECK(c.hook_count == 0);
    if (test_failures) dump(&c);
}

static void test_text_globals(void) {
    const char *src =
        "import { record } from 'axe:test';\n"
        "const enc = new TextEncoder(), dec = new TextDecoder();\n"
        "const hex = (b) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');\n"
        "record(`enc ${hex(enc.encode('h\\u00e9\\u{1F600}'))}`);\n"
        "record(`lone ${hex(enc.encode('a\\ud800b'))}`);\n"
        "const into = new Uint8Array(5);\n"
        "const r = enc.encodeInto('ab\\u{1F600}', into);\n"
        "record(`into ${r.read} ${r.written}`);\n"
        "record(`dec ${dec.decode(new Uint8Array([0x68, 0xc3, 0xa9, 0xff, 0x21]))}`);\n"
        "try { new TextDecoder('utf-8', { fatal: true }).decode(new Uint8Array([0xff])); record('no'); }\n"
        "catch (e) { record(`fatal ${e.name}`); }\n"
        "const s = new TextDecoder();\n"
        "const part = s.decode(new Uint8Array([0xef, 0xbb, 0xbf, 0x61, 0xf0, 0x9f]), { stream: true });\n"
        "record(`stream ${part}|${s.decode(new Uint8Array([0x98, 0x80]))}`);\n"
        "try { new TextDecoder('latin1'); } catch (e) { record(`label ${e.name}`); }\n"
        "record(`perf ${typeof performance.now()} ${performance.timeOrigin > 1.7e12}`);\n"
        "const t0 = performance.now(); for (let i = 0; i < 1e5; i++); record(`mono ${performance.now() >= t0}`);\n"
        "console.log('a', 1, { b: [1, 'x'] }, null, undefined, new Error('e'));\n"
        "record(`globals ${typeof require} ${typeof process} ${typeof std} ${typeof os}`);\n";
    capture c;
    run(&(run_desc){.src = src}, &c);
    CHECK(has_line(&c, "enc 68c3a9f09f9880"));
    CHECK(has_line(&c, "lone 61efbfbd62"));
    CHECK(has_line(&c, "into 2 2")); // the emoji doesn't fit the 3 bytes left
    CHECK(has_line(&c, "dec h\xC3\xA9\xEF\xBF\xBD!"));
    CHECK(has_line(&c, "fatal TypeError"));
    CHECK(has_line(&c, "stream a|\xF0\x9F\x98\x80"));
    CHECK(has_line(&c, "label RangeError"));
    CHECK(has_line(&c, "perf number true"));
    CHECK(has_line(&c, "mono true"));
    CHECK(has_line(&c, "globals undefined undefined undefined undefined")); // no OS access
    CHECK(c.hook_count == 0);
    if (test_failures) dump(&c);
}

static void test_bytecode_bundle(void) {
    const char *src = "import { record } from 'axe:test';\nrecord('from bytecode');\n";
    ui_js *js;
    CHECK(ui_js_create(&(ui_js_desc){0}, &js) == UI_OK);
    // axe:test only has to exist when the bundle runs: compiling leaves imports unresolved.
    uint8_t *code, *bundle;
    size_t code_size, bundle_size;
    CHECK(ui_js_compile_module(js, "test.js", src, strlen(src), &code, &code_size));
    ui_js_destroy(js);
    CHECK(ui_script_bundle_wrap(code, code_size, &bundle, &bundle_size) == UI_OK);
    free(code);
    capture c;
    run(&(run_desc){.bundle = bundle, .bundle_size = bundle_size}, &c);
    CHECK(has_line(&c, "from bytecode"));
    // Bytecode for another engine version is refused, not run.
    const uint8_t *body;
    size_t body_size;
    bool is_bytecode;
    const char *why;
    CHECK(ui_script_bundle_open(bundle, bundle_size, &body, &body_size, &is_bytecode, &why) == UI_OK && is_bytecode);
    bundle[8] ^= 1;
    CHECK(ui_script_bundle_open(bundle, bundle_size, &body, &body_size, &is_bytecode, &why) == UI_ERR_UNSUPPORTED);
    CHECK(why && strstr(why, "another engine"));
    bundle[8] ^= 1;
    bundle[12] ^= 1;
    CHECK(ui_script_bundle_open(bundle, bundle_size, &body, &body_size, &is_bytecode, &why) == UI_ERR_MALFORMED);
    free(bundle);
}

int main(void) {
    RUN(test_turn_order);
    RUN(test_timers_deterministic);
    RUN(test_timers_paused);
    RUN(test_errors_reported);
    RUN(test_load_errors);
    RUN(test_long_task);
    RUN(test_leak_report);
    RUN(test_commands);
    RUN(test_text_globals);
    RUN(test_bytecode_bundle);
    return test_failures ? 1 : 0;
}
