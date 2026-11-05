// What every language's runtime host shares (runtime.h): timers on the animation clock, the wire
// timer they multiplex onto, and reporting. docs/script.md, "Timers" and "Errors".
#include "script/host/runtime.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wire/gen/wire_gen.h"

#define MODULE "script"

// ---- Timers ------------------------------------------------------------------------------

void ui_script_timers_init(ui_script_timers *t) {
    memset(t, 0, sizeof *t);
    t->armed = UI_SCRIPT_NO_DEADLINE;
}

void ui_script_timers_free(ui_script_timers *t) {
    free(t->heap);
    t->heap = NULL;
    t->count = t->capacity = 0;
}

static bool timer_before(const ui_script_timer *a, const ui_script_timer *b) {
    return a->deadline < b->deadline || (a->deadline == b->deadline && a->seq < b->seq);
}

static void sift_up(ui_script_timer *t, uint32_t i) {
    while (i) {
        uint32_t parent = (i - 1) / 2;
        if (!timer_before(&t[i], &t[parent])) break;
        ui_script_timer x = t[i];
        t[i] = t[parent];
        t[parent] = x;
        i = parent;
    }
}

static void sift_down(ui_script_timer *t, uint32_t n, uint32_t i) {
    for (;;) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < n && timer_before(&t[l], &t[m])) m = l;
        if (r < n && timer_before(&t[r], &t[m])) m = r;
        if (m == i) return;
        ui_script_timer x = t[i];
        t[i] = t[m];
        t[m] = x;
        i = m;
    }
}

static bool push_timer(ui_script_timers *t, ui_script_timer timer) {
    if (ui_grow((void **)&t->heap, &t->capacity, t->count + 1, sizeof *t->heap) != UI_OK) return false;
    t->heap[t->count] = timer;
    sift_up(t->heap, t->count++);
    return true;
}

static ui_script_timer remove_timer(ui_script_timers *t, uint32_t i) {
    ui_script_timer timer = t->heap[i];
    t->heap[i] = t->heap[--t->count];
    if (i < t->count) {
        sift_down(t->heap, t->count, i);
        sift_up(t->heap, i);
    }
    return timer;
}

bool ui_script_timers_add(ui_script_timers *t, uint64_t turn_ns, double ms, bool repeat, void *fn, void *args,
                          uint32_t *id) {
    if (!(ms > 0)) ms = 0;        // NaN and negative delays are 0
    if (repeat && ms < 1) ms = 1; // an interval of 0 would fire on every core step
    double ns = ms * 1e6;
    uint64_t delay = ns >= 1.8e19 ? UINT64_MAX / 2 : (uint64_t)llround(ns);
    if (++t->next_id == 0) t->next_id = 1; // ids are never 0, as on the web
    ui_script_timer timer = {
        .deadline = turn_ns + delay < turn_ns ? UINT64_MAX - 1 : turn_ns + delay,
        .seq = t->next_seq++,
        .interval = repeat ? delay : 0,
        .id = t->next_id,
        .fn = fn,
        .args = args,
    };
    if (!push_timer(t, timer)) return false;
    *id = timer.id;
    return true;
}

bool ui_script_timers_clear(ui_script_timers *t, double id, ui_script_timer *out) {
    for (uint32_t i = 0; i < t->count; i++) {
        if ((double)t->heap[i].id != id) continue;
        *out = remove_timer(t, i);
        return true;
    }
    return false;
}

bool ui_script_timers_pop(ui_script_timers *t, uint64_t turn_ns, uint64_t batch, const ui_logger *log,
                          ui_script_timer *out) {
    if (!t->count || t->heap[0].deadline > turn_ns || t->heap[0].seq >= batch) return false;
    ui_script_timer timer = remove_timer(t, 0);
    if (timer.interval) {
        // Each deadline follows the previous one, so an interval doesn't drift; a clock that
        // jumped several periods ahead fires it once.
        ui_script_timer next = timer;
        next.deadline = timer.deadline + timer.interval;
        if (next.deadline <= turn_ns) next.deadline = turn_ns + timer.interval;
        if (!push_timer(t, next)) {
            UI_ERROR(log, MODULE, "out of memory re-arming an interval; it stops");
            timer.interval = 0;
        }
    }
    *out = timer;
    return true;
}

bool ui_script_timers_fired(ui_script_timers *t, const ui_wire_op *op) {
    if (op->opcode != UI_EVENT_TIMER_FIRED) return false;
    ui_wire_timer_fired fired;
    ui_wire_timer_fired_read(op->payload, op->payload_size, &fired);
    if (fired.id != UI_SCRIPT_TIMER_ID) return false;
    t->armed = UI_SCRIPT_NO_DEADLINE; // a fired wire timer is disarmed
    return true;
}

// Appends set_timer to the turn's buffer (making one if the language sent none).
static void append_timer(const ui_logger *log, uint64_t deadline, uint8_t **out, size_t *out_size) {
    ui_wire_set_timer op = {.id = UI_SCRIPT_TIMER_ID, .deadline_ns = deadline};
    if (!*out) {
        ui_wire_buf b;
        ui_wire_buf_init(&b, UI_WIRE_MAGIC_CMD);
        if (ui_wire_set_timer_put(&b, &op) != UI_OK) return;
        *out = ui_wire_buf_finish(&b, out_size);
        return;
    }
    size_t at = *out_size, size = at + UI_WIRE_OP_HEADER_SIZE + UI_WIRE_SET_TIMER_SIZE;
    uint8_t *grown = realloc(*out, size);
    if (!grown) {
        UI_ERROR(log, MODULE, "out of memory arming the timers");
        return;
    }
    ui_wire_put_u16(grown + at, UI_OP_SET_TIMER);
    ui_wire_put_u16(grown + at + 2, UI_WIRE_OP_HEADER_SIZE + UI_WIRE_SET_TIMER_SIZE);
    ui_wire_set_timer_write(grown + at + UI_WIRE_OP_HEADER_SIZE, &op);
    ui_wire_put_u32(grown + 8, (uint32_t)size);                   // total_bytes
    ui_wire_put_u32(grown + 12, ui_wire_get_u32(grown + 12) + 1); // op_count
    *out = grown;
    *out_size = size;
}

void ui_script_timers_arm(ui_script_timers *t, const ui_logger *log, uint8_t **out, size_t *out_size) {
    uint64_t next = t->count ? t->heap[0].deadline : UI_SCRIPT_NO_DEADLINE;
    if (next == t->armed) return;
    append_timer(log, next == UI_SCRIPT_NO_DEADLINE ? UI_WIRE_TIMER_CANCEL : next, out, out_size);
    t->armed = next;
}

// ---- The turn --------------------------------------------------------------------------------

void ui_script_turn_time(ui_wire_reader reader, uint64_t *turn_ns) {
    ui_wire_op op;
    if (ui_wire_reader_next(&reader, &op) == UI_OK && op.opcode == UI_EVENT_CLOCK &&
        ui_wire_event_framed(op.opcode, op.payload, op.payload_size)) {
        ui_wire_clock c;
        ui_wire_clock_read(op.payload, op.payload_size, &c);
        if (c.now_ns > *turn_ns) *turn_ns = c.now_ns;
    }
}

// ---- Reporting ---------------------------------------------------------------------------------

void ui_script_report(const ui_script_env *env, ui_trace_kind kind, const char *prefix, const char *name,
                      const char *message, const char *stack, const char *context) {
    (void)kind; // without hooks
    size_t n = strlen(prefix) + strlen(name) + strlen(message) + strlen(stack) + strlen(context) + 32;
    char *text = malloc(n);
    if (!text) return;
    snprintf(text, n, "%s%s%s%s (%s)%s%s", prefix, name, name[0] ? ": " : "", message, context, stack[0] ? "\n" : "",
             stack);
    size_t len = strlen(text);
    while (len && text[len - 1] == '\n') text[--len] = 0;
    UI_ERROR(env->log, MODULE, "%s", text);
    UI_HOOK_SCRIPT(env->hooks, kind, text, 0);
    free(text);
}

void ui_script_report_long_task(const ui_script_env *env, const char *what, uint64_t start_ns, uint64_t threshold_ns) {
    uint64_t took = ui_monotonic_ns() - start_ns;
    if (took <= threshold_ns) return;
    char text[160];
    snprintf(text, sizeof text, "long task: %s took %.1f ms (threshold %.1f ms)", what, (double)took / 1e6,
             (double)threshold_ns / 1e6);
    UI_WARN(env->log, MODULE, "%s", text);
    UI_HOOK_SCRIPT(env->hooks, UI_TRACE_SCRIPT_LONG_TASK, text, took);
}

// ---- Files -------------------------------------------------------------------------------------

uint8_t *ui_script_read_file(const char *path, size_t *size) {
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
