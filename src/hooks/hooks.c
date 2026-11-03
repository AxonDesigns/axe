#include "hooks/hooks_internal.h"

#include <string.h>

#include "runtime/app_internal.h"
#include "runtime/common.h"

void ui_hooks_init(ui_hooks *h) {
    memset(h->subs, 0, sizeof h->subs);
    atomic_init(&h->mask, 0u);
}

static void dispatch(ui_hooks *h, const ui_trace_event *ev) {
    uint32_t mask = atomic_load_explicit(&h->mask, memory_order_acquire);
    for (uint32_t i = 0; mask; i++, mask >>= 1) {
        if (mask & 1u) h->subs[i].fn(h->subs[i].user, ev);
    }
}

void ui_hooks_emit(ui_hooks *h, ui_trace_kind kind, ui_phase phase, ui_handle surface,
                   uint64_t frame_id) {
    ui_trace_event ev = {
        .kind = kind, .phase = phase, .surface = surface, .frame_id = frame_id,
        .time_ns = ui_monotonic_ns(),
    };
    dispatch(h, &ev);
}

void ui_hooks_emit_presented(ui_hooks *h, ui_handle surface, uint64_t frame_id,
                             uint64_t present_ns, uint64_t refresh_ns) {
    ui_trace_event ev = {
        .kind = UI_TRACE_PRESENTED, .surface = surface, .frame_id = frame_id,
        .time_ns = present_ns, .refresh_ns = refresh_ns,
    };
    dispatch(h, &ev);
}

void ui_hooks_emit_script(ui_hooks *h, ui_trace_kind kind, const char *text, uint64_t duration_ns) {
    ui_trace_event ev = {.kind = kind, .time_ns = ui_monotonic_ns(), .text = text, .duration_ns = duration_ns};
    dispatch(h, &ev);
}

ui_result ui_hooks_subscribe(ui_app *app, ui_trace_fn fn, void *user, uint32_t *out_id) {
#if UI_HOOKS_ENABLED
    if (!fn) return UI_ERR_INVALID_ARGUMENT;
    if (app->running) return UI_ERR_WRONG_STATE;
    ui_hooks *h = &app->hooks;
    uint32_t mask = atomic_load_explicit(&h->mask, memory_order_relaxed);
    for (uint32_t i = 0; i < UI_HOOKS_MAX_SUBSCRIBERS; i++) {
        if (mask & (1u << i)) continue;
        h->subs[i] = (ui_hook_sub){fn, user};
        atomic_store_explicit(&h->mask, mask | (1u << i), memory_order_release);
        if (out_id) *out_id = i;
        return UI_OK;
    }
    return UI_ERR_FULL;
#else
    (void)app;
    (void)fn;
    (void)user;
    (void)out_id;
    return UI_ERR_UNSUPPORTED;
#endif
}

void ui_hooks_unsubscribe(ui_app *app, uint32_t id) {
    if (id >= UI_HOOKS_MAX_SUBSCRIBERS || app->running) return;
    atomic_fetch_and_explicit(&app->hooks.mask, ~(1u << id), memory_order_release);
}
