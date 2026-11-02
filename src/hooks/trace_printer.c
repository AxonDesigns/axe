// UI_TRACE=1 consumer: once per second prints frame rate, frame and present intervals, and
// min/avg/max per phase. Core-thread and render-thread phases accumulate into atomics; the
// core thread prints.
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "hooks/hooks_internal.h"

typedef struct phase_acc {
    uint64_t begin_ns; // written only by the thread that runs the phase
    _Atomic uint64_t sum, count, max, min;
} phase_acc;

typedef struct interval_acc {
    uint64_t last_ns;
    uint64_t sum, count, max, min, late;
} interval_acc;

struct ui_trace_printer {
    uint32_t id;
    uint64_t window_start;
    phase_acc phases[UI_PHASE_COUNT];
    interval_acc frames;   // FRAME_BEGIN to FRAME_BEGIN (core thread)
    interval_acc presents; // compositor present timestamps (core thread)
    _Atomic uint64_t frames_done;
    uint64_t refresh_ns;
    uint64_t idle_frames;
    bool resuming; // after an idle period the next intervals measure the gap, not pacing
};

static const char *const phase_names[UI_PHASE_COUNT] = {"apply", "animate", "commit", "encode",
                                                        "present"};

static void interval_add(interval_acc *a, uint64_t t, uint64_t late_threshold) {
    if (a->last_ns && t > a->last_ns) {
        uint64_t d = t - a->last_ns;
        a->sum += d;
        a->count++;
        if (d > a->max) a->max = d;
        if (!a->min || d < a->min) a->min = d;
        if (late_threshold && d > late_threshold) a->late++;
    }
    a->last_ns = t;
}

static void atomic_min(_Atomic uint64_t *v, uint64_t x) {
    uint64_t cur = atomic_load_explicit(v, memory_order_relaxed);
    while ((!cur || x < cur) &&
           !atomic_compare_exchange_weak_explicit(v, &cur, x, memory_order_relaxed,
                                                  memory_order_relaxed)) {}
}

static void atomic_max(_Atomic uint64_t *v, uint64_t x) {
    uint64_t cur = atomic_load_explicit(v, memory_order_relaxed);
    while (x > cur && !atomic_compare_exchange_weak_explicit(v, &cur, x, memory_order_relaxed,
                                                             memory_order_relaxed)) {}
}

static double ms(uint64_t ns) { return (double)ns / 1e6; }

static void print_and_reset(ui_trace_printer *p, uint64_t now) {
    double secs = (double)(now - p->window_start) / 1e9;
    uint64_t done = atomic_exchange_explicit(&p->frames_done, 0, memory_order_relaxed);
    char line[768];
    int n = snprintf(line, sizeof line, "[trace] %.1f fps | frame %.2f/%.2f/%.2f ms",
                     (double)done / secs,
                     ms(p->frames.min), p->frames.count ? ms(p->frames.sum / p->frames.count) : 0.0,
                     ms(p->frames.max));
    if (p->presents.count) {
        n += snprintf(line + n, sizeof line - (size_t)n,
                      " | present %.2f/%.2f/%.2f ms (refresh %.2f, late %" PRIu64 ")",
                      ms(p->presents.min), ms(p->presents.sum / p->presents.count),
                      ms(p->presents.max), ms(p->refresh_ns), p->presents.late);
    }
    for (int i = 0; i < UI_PHASE_COUNT && n < (int)sizeof line; i++) {
        phase_acc *a = &p->phases[i];
        uint64_t count = atomic_exchange_explicit(&a->count, 0, memory_order_relaxed);
        uint64_t sum = atomic_exchange_explicit(&a->sum, 0, memory_order_relaxed);
        uint64_t mx = atomic_exchange_explicit(&a->max, 0, memory_order_relaxed);
        uint64_t mn = atomic_exchange_explicit(&a->min, 0, memory_order_relaxed);
        if (!count) continue;
        n += snprintf(line + n, sizeof line - (size_t)n, " | %s %.3f/%.3f/%.3f", phase_names[i],
                      ms(mn), ms(sum / count), ms(mx));
    }
    if (p->idle_frames && n < (int)sizeof line)
        n += snprintf(line + n, sizeof line - (size_t)n, " | idle %" PRIu64, p->idle_frames);
    p->idle_frames = 0;
    fprintf(stderr, "%s\n", line);
    uint64_t frames_last = p->frames.last_ns, presents_last = p->presents.last_ns;
    p->frames = (interval_acc){.last_ns = frames_last};
    p->presents = (interval_acc){.last_ns = presents_last};
    p->window_start = now;
}

static void on_trace(void *user, const ui_trace_event *ev) {
    ui_trace_printer *p = user;
    switch (ev->kind) {
    case UI_TRACE_FRAME_BEGIN:
        // Core thread.
        if (!p->window_start) p->window_start = ev->time_ns;
        if (p->resuming) {
            // Intervals spanning an idle period are not frame times or missed vblanks.
            p->resuming = false;
            p->frames.last_ns = 0;
            p->presents.last_ns = 0;
        }
        interval_add(&p->frames, ev->time_ns, 0);
        if (ev->time_ns - p->window_start >= 1000000000u) print_and_reset(p, ev->time_ns);
        break;
    case UI_TRACE_FRAME_END:
        atomic_fetch_add_explicit(&p->frames_done, 1, memory_order_relaxed);
        break;
    case UI_TRACE_PHASE_BEGIN:
        p->phases[ev->phase].begin_ns = ev->time_ns;
        break;
    case UI_TRACE_PHASE_END: {
        phase_acc *a = &p->phases[ev->phase];
        uint64_t d = ev->time_ns - a->begin_ns;
        atomic_fetch_add_explicit(&a->sum, d, memory_order_relaxed);
        atomic_fetch_add_explicit(&a->count, 1, memory_order_relaxed);
        atomic_max(&a->max, d);
        atomic_min(&a->min, d);
        break;
    }
    case UI_TRACE_FRAME_IDLE:
        // Core thread.
        p->idle_frames++;
        p->resuming = true;
        break;
    case UI_TRACE_PRESENTED:
        // Core thread (platform feedback). A present more than 1.5 refresh periods after
        // the previous one means at least one vblank was missed.
        if (!ev->time_ns) break;
        if (ev->refresh_ns) p->refresh_ns = ev->refresh_ns;
        interval_add(&p->presents, ev->time_ns, p->refresh_ns * 3 / 2);
        break;
    }
}

ui_result ui_trace_printer_install(ui_app *app, ui_trace_printer **out) {
    ui_trace_printer *p = calloc(1, sizeof *p);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    ui_result r = ui_hooks_subscribe(app, on_trace, p, &p->id);
    if (r != UI_OK) {
        free(p);
        return r;
    }
    *out = p;
    return UI_OK;
}

void ui_trace_printer_destroy(ui_app *app, ui_trace_printer *printer) {
    if (!printer) return;
    ui_hooks_unsubscribe(app, printer->id);
    free(printer);
}
