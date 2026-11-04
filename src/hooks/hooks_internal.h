// Tracepoint registry. With UI_HOOKS_ENABLED=0 every UI_HOOK_* macro expands to nothing.
// Compiled in, a tracepoint is one relaxed atomic load until someone subscribes; the
// timestamp is only taken when a subscriber exists.
#pragma once

#include <stdatomic.h>

#include "ui/hooks.h"

typedef struct ui_hook_sub {
    ui_trace_fn fn;
    void *user;
} ui_hook_sub;

typedef struct ui_hooks {
    _Atomic uint32_t mask; // bit i set: subs[i] is live
    ui_hook_sub subs[UI_HOOKS_MAX_SUBSCRIBERS];
} ui_hooks;

void ui_hooks_init(ui_hooks *h);
void ui_hooks_emit(ui_hooks *h, ui_trace_kind kind, ui_phase phase, ui_handle surface,
                   uint64_t frame_id);
void ui_hooks_emit_presented(ui_hooks *h, ui_handle surface, uint64_t frame_id,
                             uint64_t present_ns, uint64_t refresh_ns);
void ui_hooks_emit_script(ui_hooks *h, ui_trace_kind kind, const char *text, uint64_t duration_ns);
void ui_hooks_emit_at(ui_hooks *h, ui_trace_kind kind, ui_handle surface, uint64_t frame_id, uint64_t time_ns);

#ifndef UI_HOOKS_ENABLED
#define UI_HOOKS_ENABLED 1
#endif

#if UI_HOOKS_ENABLED
#define UI_HOOK_ACTIVE(h) (atomic_load_explicit(&(h)->mask, memory_order_relaxed) != 0)
#define UI_HOOK(h, kind, phase, surface, frame)                                                \
    do {                                                                                       \
        if (UI_HOOK_ACTIVE(h)) ui_hooks_emit((h), (kind), (phase), (surface), (frame));       \
    } while (0)
#define UI_HOOK_PRESENTED(h, surface, frame, present_ns, refresh_ns)                           \
    do {                                                                                       \
        if (UI_HOOK_ACTIVE(h))                                                                 \
            ui_hooks_emit_presented((h), (surface), (frame), (present_ns), (refresh_ns));      \
    } while (0)
#define UI_HOOK_SCRIPT(h, kind, text, duration_ns)                                              \
    do {                                                                                       \
        if (UI_HOOK_ACTIVE(h)) ui_hooks_emit_script((h), (kind), (text), (duration_ns));        \
    } while (0)
#define UI_HOOK_AT(h, kind, surface, frame, time_ns)                                            \
    do {                                                                                       \
        if (UI_HOOK_ACTIVE(h)) ui_hooks_emit_at((h), (kind), (surface), (frame), (time_ns));     \
    } while (0)
#else
#define UI_HOOK_AT(h, kind, surface, frame, time_ns)                                            \
    ((void)sizeof(h), (void)sizeof(surface), (void)sizeof(frame), (void)sizeof(time_ns))
#define UI_HOOK_SCRIPT(h, kind, text, duration_ns)                                              \
    ((void)sizeof(h), (void)sizeof(text), (void)sizeof(duration_ns))
// sizeof marks the arguments as used without evaluating them: no code is generated.
#define UI_HOOK(h, kind, phase, surface, frame)                                                \
    ((void)sizeof(h), (void)sizeof(surface), (void)sizeof(frame))
#define UI_HOOK_PRESENTED(h, surface, frame, present_ns, refresh_ns)                           \
    ((void)sizeof(h), (void)sizeof(surface), (void)sizeof(frame), (void)sizeof(present_ns),     \
     (void)sizeof(refresh_ns))
#endif

#define UI_HOOK_PHASE_BEGIN(h, phase, surface, frame)                                          \
    UI_HOOK(h, UI_TRACE_PHASE_BEGIN, phase, surface, frame)
#define UI_HOOK_PHASE_END(h, phase, surface, frame)                                            \
    UI_HOOK(h, UI_TRACE_PHASE_END, phase, surface, frame)

// Prints per-phase timings every second when UI_TRACE=1. Thread: main, before running.
typedef struct ui_trace_printer ui_trace_printer;
ui_result ui_trace_printer_install(ui_app *app, ui_trace_printer **out);
void ui_trace_printer_destroy(ui_app *app, ui_trace_printer *printer);
