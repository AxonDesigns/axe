// The script host (docs/script.md): an app's bundle in turns, on the logic thread.
//
// A turn handles one event buffer: the clock event sets the turn's time, the bundle runs in
// the first turn, every event goes to @axe/core's dispatcher (the clock too: the turn's
// start; the host's own timer event goes to the due JS timers instead), each followed by a
// microtask checkpoint, and the turn ends by flushing one command buffer, to which the host
// adds its timer's re-arm.
#include "script/host/host.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "unicode/utf8.h"
#include "wire/gen/wire_gen.h"

#define MODULE "script"
#define DEFAULT_LONG_TASK_NS 50000000ull
#define COMMAND_CAPACITY (64u << 10)
#define NO_DEADLINE UINT64_MAX
#define MAX_TIMER_ARGS 16u

static const char prelude_js[] = {
#embed "prelude.js"
    , 0};

typedef struct script_timer {
    uint64_t deadline, seq, interval; // interval 0: setTimeout
    uint32_t id;
    ui_js_ref *fn, *args;
} script_timer;

typedef struct rejection {
    ui_js_ref *promise, *reason;
} rejection;

struct ui_script_host {
    ui_js *js;
    ui_script_env env;
    const ui_script_internal *internal;
    uint64_t long_task_ns;
    double time_origin_ms;

    char *name;
    uint8_t *file;           // the bundle read from disk, if it came from a file
    const uint8_t *code;     // source or bytecode
    size_t code_size;
    bool bytecode;
    bool loaded;             // the first turn has run the bundle (or failed to)
    char **args;
    uint32_t arg_count;
    ui_js_ref *module_promise;

    uint64_t turn_ns;        // the animation clock of the current turn
    ui_js_ref *dispatcher, *flush;

    // The command storage JS writes into (an external ArrayBuffer over `cmd`), and the buffer
    // it submitted this turn.
    uint8_t *cmd;
    size_t cmd_capacity;
    ui_js_ref *cmd_buffer;
    uint8_t *out;
    size_t out_size;

    script_timer *timers;    // a binary heap by (deadline, seq)
    uint32_t timer_count, timer_capacity;
    uint64_t next_seq;
    uint32_t next_id;
    uint64_t armed;          // the wire timer's deadline, NO_DEADLINE when disarmed

    rejection *rejections;   // unhandled since the last checkpoint
    uint32_t rejection_count, rejection_capacity;
    uint64_t task_start;
#ifdef AXE_DEV
    ui_dev *dev;
#endif
};

ui_js *ui_script_host_engine(ui_script_host *h) { return h->js; }

// ---- Reporting ---------------------------------------------------------------------------

static void report(ui_script_host *h, ui_trace_kind kind, const char *prefix, const ui_js_exception *e,
                   const char *context) {
    const char *stack = e->stack && e->stack[0] ? e->stack : "";
    size_t n = strlen(prefix) + strlen(e->name) + strlen(e->message) + strlen(stack) + strlen(context) + 32;
    char *text = malloc(n);
    if (!text) return;
    snprintf(text, n, "%s%s%s%s (%s)%s%s", prefix, e->name, e->name[0] ? ": " : "", e->message, context,
             stack[0] ? "\n" : "", stack);
    size_t len = strlen(text);
    while (len && text[len - 1] == '\n') text[--len] = 0;
    UI_ERROR(h->env.log, MODULE, "%s", text);
    UI_HOOK_SCRIPT(h->env.hooks, kind, text, 0);
    free(text);
}

// Reports the pending exception as uncaught; the app keeps running.
static void report_exception(ui_script_host *h, const char *context) {
    ui_js_exception e;
    if (!ui_js_take_exception(h->js, &e, NULL)) return;
    report(h, UI_TRACE_SCRIPT_EXCEPTION, "Uncaught ", &e, context);
    ui_js_exception_free(&e);
}

static void on_job_error(void *user, ui_js *js) {
    (void)js;
    report_exception(user, "in a microtask");
}

static void on_rejection(void *user, ui_js *js, ui_js_val promise, ui_js_val reason, bool handled) {
    ui_script_host *h = user;
    if (handled) {
        for (uint32_t i = 0; i < h->rejection_count; i++) {
            ui_js_val p;
            if (!ui_js_ref_get(js, h->rejections[i].promise, &p) || !ui_js_strict_equal(js, p, promise)) continue;
            ui_js_ref_free(js, h->rejections[i].promise);
            ui_js_ref_free(js, h->rejections[i].reason);
            h->rejections[i] = h->rejections[--h->rejection_count];
            return;
        }
        return;
    }
    if (ui_grow((void **)&h->rejections, &h->rejection_capacity, h->rejection_count + 1, sizeof *h->rejections) !=
        UI_OK)
        return;
    rejection r = {ui_js_ref_new(js, promise, false), ui_js_ref_new(js, reason, false)};
    if (r.promise && r.reason) h->rejections[h->rejection_count++] = r;
    else ui_js_ref_free(js, r.promise), ui_js_ref_free(js, r.reason);
}

// Rejections still unhandled at a checkpoint are reported, as the web does after a task.
static void report_rejections(ui_script_host *h) {
    ui_js *js = h->js;
    while (h->rejection_count) {
        rejection r = h->rejections[--h->rejection_count];
        uint32_t scope = ui_js_scope_open(js);
        ui_js_val promise, reason, module;
        bool is_module = h->module_promise && ui_js_ref_get(js, h->module_promise, &module) &&
                         ui_js_ref_get(js, r.promise, &promise) && ui_js_strict_equal(js, promise, module);
        if (ui_js_ref_get(js, r.reason, &reason)) {
            ui_js_exception e;
            ui_js_describe(js, reason, &e);
            if (is_module) report(h, UI_TRACE_SCRIPT_EXCEPTION, "Uncaught ", &e, "evaluating the app");
            else report(h, UI_TRACE_SCRIPT_REJECTION, "Uncaught (in promise) ", &e, "unhandled rejection");
            ui_js_exception_free(&e);
        }
        ui_js_scope_close(js, scope);
        ui_js_ref_free(js, r.promise);
        ui_js_ref_free(js, r.reason);
    }
}

// ---- Tasks ---------------------------------------------------------------------------------

static void task_begin(ui_script_host *h) { h->task_start = ui_monotonic_ns(); }

// A task ends with a microtask checkpoint; then unhandled rejections and long tasks are
// reported.
static void task_end(ui_script_host *h, const char *what) {
    ui_js_run_microtasks(h->js, on_job_error, h);
    report_rejections(h);
    uint64_t took = ui_monotonic_ns() - h->task_start;
    if (took <= h->long_task_ns) return;
    char text[160];
    snprintf(text, sizeof text, "long task: %s took %.1f ms (threshold %.1f ms)", what, (double)took / 1e6,
             (double)h->long_task_ns / 1e6);
    UI_WARN(h->env.log, MODULE, "%s", text);
    UI_HOOK_SCRIPT(h->env.hooks, UI_TRACE_SCRIPT_LONG_TASK, text, took);
}

// ---- Timers ------------------------------------------------------------------------------

static bool timer_before(const script_timer *a, const script_timer *b) {
    return a->deadline < b->deadline || (a->deadline == b->deadline && a->seq < b->seq);
}

static void sift_up(script_timer *t, uint32_t i) {
    while (i) {
        uint32_t parent = (i - 1) / 2;
        if (!timer_before(&t[i], &t[parent])) break;
        script_timer x = t[i];
        t[i] = t[parent];
        t[parent] = x;
        i = parent;
    }
}

static void sift_down(script_timer *t, uint32_t n, uint32_t i) {
    for (;;) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < n && timer_before(&t[l], &t[m])) m = l;
        if (r < n && timer_before(&t[r], &t[m])) m = r;
        if (m == i) return;
        script_timer x = t[i];
        t[i] = t[m];
        t[m] = x;
        i = m;
    }
}

static bool push_timer(ui_script_host *h, script_timer t) {
    if (ui_grow((void **)&h->timers, &h->timer_capacity, h->timer_count + 1, sizeof *h->timers) != UI_OK) return false;
    h->timers[h->timer_count] = t;
    sift_up(h->timers, h->timer_count++);
    return true;
}

static script_timer remove_timer(ui_script_host *h, uint32_t i) {
    script_timer t = h->timers[i];
    h->timers[i] = h->timers[--h->timer_count];
    if (i < h->timer_count) {
        sift_down(h->timers, h->timer_count, i);
        sift_up(h->timers, i);
    }
    return t;
}

static void free_timer(ui_script_host *h, script_timer *t) {
    ui_js_ref_free(h->js, t->fn);
    ui_js_ref_free(h->js, t->args);
}

// Runs the timers due at the turn's time, earliest first (ties in creation order). Timers
// set by these callbacks wait for a later turn, even with a 0 delay.
static void run_timers(ui_script_host *h) {
    ui_js *js = h->js;
    uint64_t batch = h->next_seq;
    while (h->timer_count && h->timers[0].deadline <= h->turn_ns && h->timers[0].seq < batch) {
        script_timer t = remove_timer(h, 0);
        if (t.interval) {
            // Each deadline follows the previous one, so an interval doesn't drift; a clock
            // that jumped several periods ahead fires it once.
            script_timer next = t;
            next.deadline = t.deadline + t.interval;
            if (next.deadline <= h->turn_ns) next.deadline = h->turn_ns + t.interval;
            if (!push_timer(h, next)) {
                UI_ERROR(h->env.log, MODULE, "out of memory re-arming an interval; it stops");
                t.interval = 0;
            }
        }
        uint32_t scope = ui_js_scope_open(js);
        ui_js_val fn, args, argv[MAX_TIMER_ARGS];
        uint32_t argc = 0;
        bool ok = ui_js_ref_get(js, t.fn, &fn) && ui_js_ref_get(js, t.args, &args) && ui_js_length(js, args, &argc);
        if (argc > MAX_TIMER_ARGS) argc = MAX_TIMER_ARGS;
        for (uint32_t i = 0; ok && i < argc; i++) ok = ui_js_get_index(js, args, i, &argv[i]);
        task_begin(h);
        ui_js_val r;
        if (ok) ok = ui_js_call(js, fn, ui_js_undefined(js), argc, argv, &r);
        if (!ok) report_exception(h, t.interval ? "in a setInterval callback" : "in a setTimeout callback");
        task_end(h, "a timer");
        ui_js_scope_close(js, scope);
        if (!t.interval) free_timer(h, &t);
    }
}

// ---- Command storage ---------------------------------------------------------------------

static ui_js_val current_buffer(ui_script_host *h) {
    ui_js_val ab;
    if (h->cmd_buffer && ui_js_ref_get(h->js, h->cmd_buffer, &ab)) return ab;
    if (!h->cmd) {
        h->cmd = malloc(COMMAND_CAPACITY);
        if (!h->cmd) return ui_js_throw_error(h->js, UI_JS_RANGE_ERROR, "out of memory for commands");
        h->cmd_capacity = COMMAND_CAPACITY;
    }
    // The host owns the memory: no free function. Every buffer handed out is detached before
    // its memory moves or leaves.
    ab = ui_js_array_buffer_external(h->js, h->cmd, h->cmd_capacity, NULL, NULL);
    if (ui_js_is_empty(ab)) return ab;
    ui_js_ref_free(h->js, h->cmd_buffer);
    h->cmd_buffer = ui_js_ref_new(h->js, ab, false);
    return ab;
}

static void detach_buffer(ui_script_host *h) {
    ui_js_val ab;
    if (h->cmd_buffer && ui_js_ref_get(h->js, h->cmd_buffer, &ab)) ui_js_array_buffer_detach(h->js, ab);
    ui_js_ref_free(h->js, h->cmd_buffer);
    h->cmd_buffer = NULL;
}

// Appends set_timer to the turn's buffer (making one if JS sent none).
static void append_timer(ui_script_host *h, uint64_t deadline) {
    ui_wire_set_timer op = {.id = UI_SCRIPT_TIMER_ID, .deadline_ns = deadline};
    if (!h->out) {
        ui_wire_buf b;
        ui_wire_buf_init(&b, UI_WIRE_MAGIC_CMD);
        if (ui_wire_set_timer_put(&b, &op) != UI_OK) return;
        h->out = ui_wire_buf_finish(&b, &h->out_size);
        return;
    }
    size_t at = h->out_size, size = at + UI_WIRE_OP_HEADER_SIZE + UI_WIRE_SET_TIMER_SIZE;
    uint8_t *grown = realloc(h->out, size);
    if (!grown) {
        UI_ERROR(h->env.log, MODULE, "out of memory arming the timers");
        return;
    }
    ui_wire_put_u16(grown + at, UI_OP_SET_TIMER);
    ui_wire_put_u16(grown + at + 2, UI_WIRE_OP_HEADER_SIZE + UI_WIRE_SET_TIMER_SIZE);
    ui_wire_set_timer_write(grown + at + UI_WIRE_OP_HEADER_SIZE, &op);
    ui_wire_put_u32(grown + 8, (uint32_t)size);                 // total_bytes
    ui_wire_put_u32(grown + 12, ui_wire_get_u32(grown + 12) + 1); // op_count
    h->out = grown;
    h->out_size = size;
}

// ---- Natives: the prelude's ----------------------------------------------------------------

static ui_script_host *host_of(const ui_js_call_info *call) { return call->data; }

static double arg_number(ui_js *js, const ui_js_call_info *call, uint32_t i, bool *ok) {
    double d = NAN;
    if (i < call->argc && !ui_js_to_number(js, call->args[i], &d)) *ok = false;
    return d;
}

static ui_js_val native_print(ui_js *js, const ui_js_call_info *call) {
    ui_script_host *h = host_of(call);
    bool ok = true;
    double level = arg_number(js, call, 0, &ok);
    char *text = ok && call->argc > 1 ? ui_js_to_utf8(js, call->args[1], NULL) : NULL;
    if (!text) return ok ? ui_js_undefined(js) : (ui_js_val){0};
    static const ui_log_level levels[] = {UI_LOG_DEBUG, UI_LOG_INFO, UI_LOG_WARN, UI_LOG_ERROR};
    ui_log_level lvl = levels[level >= 0 && level <= 3 ? (int)level : 1];
    UI_LOG(h->env.log, lvl, "js", "%s", text);
    free(text);
    return ui_js_undefined(js);
}

static ui_js_val native_set_timer(ui_js *js, const ui_js_call_info *call) {
    ui_script_host *h = host_of(call);
    bool ok = true;
    double ms = arg_number(js, call, 1, &ok);
    if (!ok) return (ui_js_val){0};
    if (!(ms > 0)) ms = 0; // NaN and negative delays are 0
    bool repeat = call->argc > 3 && ui_js_truthy(js, call->args[3]);
    if (repeat && ms < 1) ms = 1; // an interval of 0 would fire on every core step
    double ns = ms * 1e6;
    uint64_t delay = ns >= 1.8e19 ? UINT64_MAX / 2 : (uint64_t)llround(ns);
    if (++h->next_id == 0) h->next_id = 1; // ids are never 0, as on the web
    script_timer t = {
        .deadline = h->turn_ns + delay < h->turn_ns ? UINT64_MAX - 1 : h->turn_ns + delay,
        .seq = h->next_seq++,
        .interval = repeat ? delay : 0,
        .id = h->next_id,
        .fn = ui_js_ref_new(js, call->args[0], false),
        .args = call->argc > 2 ? ui_js_ref_new(js, call->args[2], false) : NULL,
    };
    if (!t.args) t.args = ui_js_ref_new(js, ui_js_array(js), false);
    if (!t.fn || !t.args || !push_timer(h, t)) {
        free_timer(h, &t);
        return ui_js_throw_error(js, UI_JS_RANGE_ERROR, "out of memory for a timer");
    }
    return ui_js_number(js, t.id);
}

static ui_js_val native_clear_timer(ui_js *js, const ui_js_call_info *call) {
    ui_script_host *h = host_of(call);
    bool ok = true;
    double id = arg_number(js, call, 0, &ok);
    if (!ok) return (ui_js_val){0};
    for (uint32_t i = 0; i < h->timer_count; i++) {
        if ((double)h->timers[i].id != id) continue;
        script_timer t = remove_timer(h, i);
        free_timer(h, &t);
        break;
    }
    return ui_js_undefined(js);
}

static ui_js_val native_enqueue(ui_js *js, const ui_js_call_info *call) {
    if (!call->argc || !ui_js_enqueue_microtask(js, call->args[0])) return (ui_js_val){0};
    return ui_js_undefined(js);
}

// UTF-8 as TextEncoder makes it: the engine writes a lone surrogate as its 3-byte (WTF-8)
// form, which becomes U+FFFD (EF BF BD), the same length.
static char *utf8_of(ui_js *js, ui_js_val v, size_t *len) {
    char *s = ui_js_to_utf8(js, v, len);
    if (!s) return NULL;
    uint8_t *p = (uint8_t *)s;
    for (size_t i = 0; i + 2 < *len; i++) {
        if (p[i] == 0xED && p[i + 1] >= 0xA0) {
            p[i] = 0xEF, p[i + 1] = 0xBF, p[i + 2] = 0xBD;
            i += 2;
        }
    }
    return s;
}

static ui_js_val native_encode(ui_js *js, const ui_js_call_info *call) {
    size_t len;
    char *s = call->argc ? utf8_of(js, call->args[0], &len) : NULL;
    if (!s) return call->argc ? (ui_js_val){0} : ui_js_array_buffer_copy(js, "", 0);
    ui_js_val ab = ui_js_array_buffer_copy(js, s, len);
    free(s);
    return ab;
}

static ui_js_val native_encode_into(ui_js *js, const ui_js_call_info *call) {
    if (call->argc < 2) return ui_js_throw_error(js, UI_JS_TYPE_ERROR, "encodeInto needs a string and a Uint8Array");
    void *dst;
    size_t cap, len;
    if (!ui_js_view_data(js, call->args[1], &dst, &cap))
        return ui_js_throw_error(js, UI_JS_TYPE_ERROR, "encodeInto: the destination must be a Uint8Array");
    char *s = utf8_of(js, call->args[0], &len);
    if (!s) return (ui_js_val){0};
    // Whole characters only; `read` counts UTF-16 code units (2 for a 4-byte sequence).
    size_t written = 0, read = 0;
    while (written < len) {
        uint32_t n = ui_utf8_seq_len((uint8_t)s[written]);
        if (written + n > cap) break;
        written += n;
        read += n == 4 ? 2 : 1;
    }
    memcpy(dst, s, written);
    free(s);
    ui_js_val out = ui_js_array(js);
    if (ui_js_is_empty(out) || !ui_js_set_index(js, out, 0, ui_js_number(js, (double)read)) ||
        !ui_js_set_index(js, out, 1, ui_js_number(js, (double)written)))
        return (ui_js_val){0};
    return out;
}

static ui_js_val native_decode(ui_js *js, const ui_js_call_info *call) {
    void *data;
    size_t size;
    if (!call->argc || !ui_js_view_data(js, call->args[0], &data, &size))
        return ui_js_throw_error(js, UI_JS_TYPE_ERROR, "decode needs a Uint8Array");
    bool ok = true;
    double end = arg_number(js, call, 1, &ok);
    if (!ok) return (ui_js_val){0};
    if (end >= 0 && end < (double)size) size = (size_t)end;
    bool fatal = call->argc > 2 && ui_js_truthy(js, call->args[2]);
    if (fatal && !ui_utf8_valid(data, size))
        return ui_js_throw_error(js, UI_JS_TYPE_ERROR, "TextDecoder: the data is not valid UTF-8");
    uint8_t *clean;
    size_t n;
    uint32_t replaced;
    if (ui_utf8_sanitize(data, size, &clean, &n, &replaced) != UI_OK)
        return ui_js_throw_error(js, UI_JS_RANGE_ERROR, "out of memory decoding text");
    ui_js_val s = ui_js_string(js, (const char *)clean, n);
    free(clean);
    return s;
}

static ui_js_val native_now(ui_js *js, const ui_js_call_info *call) {
    ui_script_host *h = host_of(call);
    if (h->internal && h->internal->performance_from_animation_clock) return ui_js_number(js, (double)h->turn_ns / 1e6);
    return ui_js_number(js, (double)(ui_monotonic_ns() - h->env.start_ns) / 1e6);
}

// ---- Natives: axe:host --------------------------------------------------------------------

static ui_js_val set_hook(ui_js *js, const ui_js_call_info *call, ui_js_ref **slot) {
    if (!call->argc || ui_js_typeof(js, call->args[0]) != UI_JS_FUNCTION)
        return ui_js_throw_error(js, UI_JS_TYPE_ERROR, "expected a function");
    ui_js_ref *r = ui_js_ref_new(js, call->args[0], false);
    if (!r) return ui_js_throw_error(js, UI_JS_RANGE_ERROR, "out of memory");
    ui_js_ref_free(js, *slot);
    *slot = r;
    return ui_js_undefined(js);
}

static ui_js_val native_set_dispatcher(ui_js *js, const ui_js_call_info *call) {
    return set_hook(js, call, &host_of(call)->dispatcher);
}

static ui_js_val native_set_flush(ui_js *js, const ui_js_call_info *call) {
    return set_hook(js, call, &host_of(call)->flush);
}

static ui_js_val native_command_buffer(ui_js *js, const ui_js_call_info *call) {
    (void)js;
    return current_buffer(host_of(call));
}

static ui_js_val native_grow_commands(ui_js *js, const ui_js_call_info *call) {
    ui_script_host *h = host_of(call);
    bool ok = true;
    double min = arg_number(js, call, 0, &ok);
    if (!ok) return (ui_js_val){0};
    if (!(min > 0) || min > (double)UI_WIRE_MAX_BUFFER_SIZE)
        return ui_js_throw_error(js, UI_JS_RANGE_ERROR, "growCommandBuffer: %g bytes is out of range", min);
    size_t cap = h->cmd_capacity ? h->cmd_capacity : COMMAND_CAPACITY;
    while (cap < (size_t)min) cap *= 2;
    if (cap > UI_WIRE_MAX_BUFFER_SIZE) cap = UI_WIRE_MAX_BUFFER_SIZE;
    if (cap <= h->cmd_capacity && h->cmd) return current_buffer(h);
    detach_buffer(h); // before the memory moves
    uint8_t *grown = realloc(h->cmd, cap);
    if (!grown) return ui_js_throw_error(js, UI_JS_RANGE_ERROR, "out of memory for commands");
    h->cmd = grown;
    h->cmd_capacity = cap;
    return current_buffer(h);
}

static ui_js_val native_submit(ui_js *js, const ui_js_call_info *call) {
    ui_script_host *h = host_of(call);
    bool ok = true;
    double size = arg_number(js, call, 0, &ok);
    if (!ok) return (ui_js_val){0};
    if (h->out) return ui_js_throw_error(js, UI_JS_ERROR, "submitCommands: one command buffer per turn");
    if (!h->cmd || !(size >= UI_WIRE_HEADER_SIZE) || size > (double)h->cmd_capacity || fmod(size, 4) != 0)
        return ui_js_throw_error(js, UI_JS_RANGE_ERROR, "submitCommands: %g bytes is not a finished buffer", size);
    detach_buffer(h);
    h->out = h->cmd; // the core takes this memory
    h->out_size = (size_t)size;
    h->cmd = NULL;
    h->cmd_capacity = 0;
    return current_buffer(h);
}

static ui_js_val native_animation_time(ui_js *js, const ui_js_call_info *call) {
    return ui_js_number(js, (double)host_of(call)->turn_ns);
}

static ui_js_val native_report_leak(ui_js *js, const ui_js_call_info *call) {
    ui_script_host *h = host_of(call);
    bool ok = true;
    double index = arg_number(js, call, 0, &ok), gen = arg_number(js, call, 1, &ok), nodes = arg_number(js, call, 2, &ok);
    if (!ok) return (ui_js_val){0};
    char text[256];
    snprintf(text, sizeof text,
             "leaked: node %08x:%u%s became unreachable from JS without destroy(); it stays in the "
             "scene until an ancestor is destroyed",
             (uint32_t)index, (uint32_t)gen, nodes > 1 ? " and its subtree" : "");
    UI_WARN(h->env.log, MODULE, "%s", text);
    UI_HOOK_SCRIPT(h->env.hooks, UI_TRACE_SCRIPT_LEAK, text, 0);
    return ui_js_undefined(js);
}

// reportError(value, context): reports `value` as an uncaught exception (stderr and
// UI_TRACE_SCRIPT_EXCEPTION) without throwing, for errors @axe/ui catches with no error boundary
// to take them. `context` ("in an effect of Card") goes where the host's own say "in a timer".
static ui_js_val native_report_error(ui_js *js, const ui_js_call_info *call) {
    ui_script_host *h = host_of(call);
    char *context = call->argc > 1 ? ui_js_to_utf8(js, call->args[1], NULL) : NULL;
    ui_js_throw(js, call->argc > 0 ? call->args[0] : ui_js_undefined(js));
    report_exception(h, context && context[0] ? context : "reported");
    free(context);
    return ui_js_undefined(js);
}

typedef struct native_entry {
    const char *name;
    uint32_t length;
    ui_js_native fn;
} native_entry;

static bool add_natives(ui_script_host *h, ui_js_val obj, const native_entry *e, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!ui_js_set(h->js, obj, e[i].name, ui_js_function(h->js, e[i].name, e[i].length, e[i].fn, h))) return false;
    return true;
}

// The prelude (console, timers, TextEncoder, performance) and the `axe:host` module.
static bool install(ui_script_host *h) {
    ui_js *js = h->js;
    static const native_entry prelude[] = {
        {"print", 2, native_print},     {"setTimer", 4, native_set_timer}, {"clearTimer", 1, native_clear_timer},
        {"enqueue", 1, native_enqueue}, {"encode", 1, native_encode},      {"encodeInto", 2, native_encode_into},
        {"decode", 3, native_decode},   {"now", 0, native_now},
    };
    ui_js_val natives = ui_js_object(js), fn, r;
    bool ok = add_natives(h, natives, prelude, UI_ARRAY_LEN(prelude)) &&
              ui_js_set(js, natives, "timeOrigin", ui_js_number(js, h->time_origin_ms)) &&
              ui_js_eval_script(js, "axe:prelude", prelude_js, sizeof prelude_js - 1, &fn) &&
              ui_js_call(js, fn, ui_js_undefined(js), 1, &natives, &r);

    static const native_entry host_fns[] = {
        {"setDispatcher", 1, native_set_dispatcher}, {"setFlush", 1, native_set_flush},
        {"commandBuffer", 0, native_command_buffer}, {"growCommandBuffer", 1, native_grow_commands},
        {"submitCommands", 1, native_submit},        {"animationTime", 0, native_animation_time},
        {"reportLeak", 3, native_report_leak},       {"reportError", 2, native_report_error},
    };
    ui_js_val exports = ui_js_object(js), args = ui_js_array(js);
    ok = ok && add_natives(h, exports, host_fns, UI_ARRAY_LEN(host_fns));
#ifdef NDEBUG
    ok = ok && ui_js_set(js, exports, "debug", ui_js_bool(js, false));
#else
    ok = ok && ui_js_set(js, exports, "debug", ui_js_bool(js, true));
#endif
    for (uint32_t i = 0; ok && i < h->arg_count; i++)
        ok = ui_js_set_index(js, args, i, ui_js_string(js, h->args[i], strlen(h->args[i])));
#ifdef AXE_DEV
    if (h->dev) ok = ok && ui_dev_install(h->dev, exports);
#endif
    ok = ok && ui_js_set(js, exports, "args", args) && ui_js_define_module(js, "axe:host", exports);
    return ok;
}

// ---- Lifecycle ---------------------------------------------------------------------------

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *data = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long n = ftell(f);
        if (n >= 0 && fseek(f, 0, SEEK_SET) == 0 && (data = malloc((size_t)n + 1))) {
            if (fread(data, 1, (size_t)n, f) != (size_t)n) {
                free(data);
                data = NULL;
            }
            *size = (size_t)n;
        }
    }
    fclose(f);
    return data;
}

#ifdef AXE_DEV
static char *dev_resolve(void *user, const char *base, const char *specifier) {
    return ui_dev_resolve(((ui_script_host *)user)->dev, base, specifier);
}

static bool dev_load(void *user, const char *name, ui_js_module_source *out) {
    return ui_dev_load(((ui_script_host *)user)->dev, name, out);
}
#endif

ui_result ui_script_host_create(const ui_script_desc *desc, const ui_script_env *env, ui_script_host **out) {
    ui_script_host *h = calloc(1, sizeof *h);
    if (!h) return UI_ERR_OUT_OF_MEMORY;
    h->env = *env;
    h->internal = desc->internal;
    h->long_task_ns = desc->long_task_ns ? desc->long_task_ns : DEFAULT_LONG_TASK_NS;
    h->armed = NO_DEADLINE;
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    double since_start = (double)(ui_monotonic_ns() - env->start_ns) / 1e6;
    h->time_origin_ms = (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6 - since_start;

    const char *name = desc->bundle_name ? desc->bundle_name : desc->bundle_path ? desc->bundle_path : "app.js";
    ui_result r = (h->name = strdup(name)) ? UI_OK : UI_ERR_OUT_OF_MEMORY;
    if (r == UI_OK && desc->arg_count) {
        h->args = calloc(desc->arg_count, sizeof *h->args);
        r = h->args ? UI_OK : UI_ERR_OUT_OF_MEMORY;
        for (uint32_t i = 0; r == UI_OK && i < desc->arg_count; i++, h->arg_count++)
            if (!(h->args[i] = strdup(desc->args[i] ? desc->args[i] : ""))) r = UI_ERR_OUT_OF_MEMORY;
    }
    const uint8_t *bytes = desc->bundle;
    size_t size = desc->bundle_size;
    bool has_bundle = true;
#ifdef AXE_DEV
    ui_dev_channel *channel = h->internal ? h->internal->dev : NULL;
    if (channel) has_bundle = false;
#endif
    if (r == UI_OK && has_bundle && desc->bundle_path) {
        bytes = h->file = read_file(desc->bundle_path, &size);
        if (!h->file) {
            UI_ERROR(env->log, MODULE, "cannot read the bundle %s", desc->bundle_path);
            r = UI_ERR_IO;
        }
    }
    if (r == UI_OK && has_bundle && !bytes) r = UI_ERR_INVALID_ARGUMENT;
    if (r == UI_OK && has_bundle) {
        const char *message;
        r = ui_script_bundle_open(bytes, size, &h->code, &h->code_size, &h->bytecode, &message);
        if (r != UI_OK) UI_ERROR(env->log, MODULE, "%s: %s", name, message);
    }
    ui_js_desc engine = {.memory_limit = desc->memory_limit, .user = h, .rejection = on_rejection};
#ifdef AXE_DEV
    // The hooks reach the dev table through the host, which creates it after the engine.
    if (channel) {
        engine.resolve = dev_resolve;
        engine.load = dev_load;
    }
#endif
    if (r == UI_OK) r = ui_js_create(&engine, &h->js);
#ifdef AXE_DEV
    if (r == UI_OK && channel) r = ui_dev_create(channel, h->js, &h->dev);
#endif
    if (r == UI_OK) {
        uint32_t scope = ui_js_scope_open(h->js);
        if (!install(h)) {
            report_exception(h, "installing the host API");
            r = UI_ERR_PLATFORM;
        }
        if (r == UI_OK && h->internal && h->internal->setup) h->internal->setup(h->internal->user, h->js);
        ui_js_scope_close(h->js, scope);
    }
    if (r != UI_OK) {
        ui_script_host_destroy(h);
        return r;
    }
    *out = h;
    return UI_OK;
}

void ui_script_host_destroy(ui_script_host *h) {
    if (!h) return;
    if (h->js) {
        for (uint32_t i = 0; i < h->timer_count; i++) free_timer(h, &h->timers[i]);
        for (uint32_t i = 0; i < h->rejection_count; i++) {
            ui_js_ref_free(h->js, h->rejections[i].promise);
            ui_js_ref_free(h->js, h->rejections[i].reason);
        }
        uint32_t scope = ui_js_scope_open(h->js);
        detach_buffer(h);
        ui_js_scope_close(h->js, scope);
        ui_js_ref_free(h->js, h->dispatcher);
        ui_js_ref_free(h->js, h->flush);
        ui_js_ref_free(h->js, h->module_promise);
#ifdef AXE_DEV
        ui_dev_release(h->dev);
#endif
        ui_js_destroy(h->js);
    }
#ifdef AXE_DEV
    ui_dev_destroy(h->dev);
#endif
    for (uint32_t i = 0; i < h->arg_count; i++) free(h->args[i]);
    free(h->args);
    free(h->timers);
    free(h->rejections);
    free(h->cmd);
    free(h->out);
    free(h->file);
    free(h->name);
    free(h);
}

// ---- The turn ------------------------------------------------------------------------------

static const char *event_name(uint16_t type) {
    switch (type) {
    case UI_EVENT_FRAME_COMMITTED: return "event frame_committed";
    case UI_EVENT_SURFACE_RESIZED: return "event surface_resized";
    case UI_EVENT_SURFACE_CLOSE_REQUESTED: return "event surface_close_requested";
    case UI_EVENT_TIMER_FIRED: return "event timer_fired";
    case UI_EVENT_FONT_LOADED: return "event font_loaded";
    case UI_EVENT_LAYOUT_MEASURED: return "event layout_measured";
    case UI_EVENT_CLOCK: return "the turn's start";
    default: return "an event";
    }
}

// The app's code: evaluated in the first turn, once the turn's time is known.
static void load(ui_script_host *h) {
    h->loaded = true;
    ui_js_module_source src = {h->code, h->code_size, h->bytecode};
    const char *name = h->name;
#ifdef AXE_DEV
    // The hot runtime comes first, and it evaluates the app: an entry that doesn't build has
    // something to show the error.
    if (h->dev) {
        const char *bootstrap = ui_dev_bootstrap(h->dev);
        src = (ui_js_module_source){bootstrap, strlen(bootstrap), false};
        name = "app:/__bootstrap__";
    }
#endif
    ui_js_val promise;
    task_begin(h);
    if (ui_js_eval_module(h->js, name, &src, &promise)) h->module_promise = ui_js_ref_new(h->js, promise, false);
    else report_exception(h, "loading the app");
    task_end(h, "evaluating the app");
}

static void dispatch(ui_script_host *h, ui_js_val events, const ui_wire_op *op, const uint8_t *base) {
    ui_js *js = h->js;
    ui_js_val fn;
    if (!h->dispatcher || !ui_js_ref_get(js, h->dispatcher, &fn)) return;
    const char *what = event_name(op->opcode);
    ui_js_val argv[4] = {events, ui_js_number(js, op->opcode), ui_js_number(js, (double)(op->payload - base)),
                         ui_js_number(js, op->payload_size)}, r;
    task_begin(h);
    if (!ui_js_call(js, fn, ui_js_undefined(js), 4, argv, &r)) report_exception(h, what);
    task_end(h, what);
}

void ui_script_host_turn(ui_script_host *h, const uint8_t *events, size_t size, uint8_t **cmds, size_t *cmds_size) {
    ui_js *js = h->js;
    *cmds = NULL;
    *cmds_size = 0;
    uint32_t scope = ui_js_scope_open(js);
    ui_wire_reader rd;
    ui_wire_op op;
    if (ui_wire_reader_init(&rd, UI_WIRE_MAGIC_EVENT, events, size) != UI_OK) {
        UI_ERROR(h->env.log, MODULE, "malformed event buffer (%zu bytes)", size);
        ui_js_scope_close(js, scope);
        return;
    }
    // The turn's time: the clock event first in the buffer (docs/command-buffer.md).
    ui_wire_reader peek = rd;
    if (ui_wire_reader_next(&peek, &op) == UI_OK && op.opcode == UI_EVENT_CLOCK &&
        ui_wire_event_framed(op.opcode, op.payload, op.payload_size)) {
        ui_wire_clock c;
        ui_wire_clock_read(op.payload, op.payload_size, &c);
        if (c.now_ns > h->turn_ns) h->turn_ns = c.now_ns;
    }
    if (!h->loaded) load(h);
#ifdef AXE_DEV
    // Updates apply at the turn's start, before its events, as a task of their own.
    bool hot_update = false;
    if (h->dev && ui_dev_take(h->dev)) {
        hot_update = true;
        task_begin(h);
        if (!ui_dev_deliver(h->dev)) report_exception(h, "applying a hot update");
        task_end(h, "applying a hot update");
    }
#endif

    // Zero copy: JS reads the buffer in place; it is detached when the turn ends.
    ui_js_val ab = ui_js_array_buffer_external(js, (void *)(uintptr_t)events, size, NULL, NULL);
    ui_result res;
    while ((res = ui_wire_reader_next(&rd, &op)) == UI_OK) {
        if (!ui_wire_event_framed(op.opcode, op.payload, op.payload_size)) {
            res = UI_ERR_MALFORMED;
            break;
        }
        if (op.opcode == UI_EVENT_TIMER_FIRED) {
            ui_wire_timer_fired t;
            ui_wire_timer_fired_read(op.payload, op.payload_size, &t);
            if (t.id == UI_SCRIPT_TIMER_ID) {
                h->armed = NO_DEADLINE; // a fired wire timer is disarmed
                run_timers(h);
                continue;
            }
        }
        if (!ui_js_is_empty(ab)) dispatch(h, ab, &op, events);
    }
    if (res != UI_ERR_END) UI_ERROR(h->env.log, MODULE, "malformed event buffer: the rest of it is skipped");
    if (!ui_js_is_empty(ab)) ui_js_array_buffer_detach(js, ab);

    // One command buffer per turn: what JS wrote, then the wire timer for the next JS timer.
    ui_js_val fn, r;
    if (h->flush && ui_js_ref_get(js, h->flush, &fn)) {
        task_begin(h);
        if (!ui_js_call(js, fn, ui_js_undefined(js), 0, NULL, &r)) report_exception(h, "flushing commands");
        task_end(h, "flushing commands");
    }
    uint64_t next = h->timer_count ? h->timers[0].deadline : NO_DEADLINE;
    if (next != h->armed) {
        append_timer(h, next == NO_DEADLINE ? UI_WIRE_TIMER_CANCEL : next);
        h->armed = next;
    }
#ifdef AXE_DEV
    if (hot_update) {
        uint64_t event_ns = ui_dev_take_event_ns(h->dev);
        UI_HOOK_SCRIPT(h->env.hooks, UI_TRACE_SCRIPT_HOT_UPDATE, "", event_ns ? ui_monotonic_ns() - event_ns : 0);
    }
#endif
    ui_js_scope_close(js, scope);
    *cmds = h->out;
    *cmds_size = h->out_size;
    h->out = NULL;
    h->out_size = 0;
}
