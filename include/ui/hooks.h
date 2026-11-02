// Hook API v0: tracepoints for frames and frame phases.
//
// Tracepoints cost nothing when the library is built with UI_HOOKS=OFF, and one relaxed
// atomic load when built in but nobody subscribes. Callbacks run synchronously on the
// thread that hits the tracepoint (core or render), so they must be short and thread-safe.
#pragma once

#include <stdint.h>

#include "ui/handle.h"
#include "ui/result.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_HOOKS_VERSION 0u
#define UI_HOOKS_MAX_SUBSCRIBERS 8u

typedef struct ui_app ui_app;

typedef enum ui_phase {
    UI_PHASE_APPLY = 0, // core: apply command buffers
    UI_PHASE_ANIMATE,   // core: tick springs
    UI_PHASE_COMMIT,    // core: build and publish display lists
    UI_PHASE_ENCODE,    // render: acquire the target, record and submit GPU work
    UI_PHASE_PRESENT,   // render: per-commit platform state and the present call
    UI_PHASE_COUNT,
} ui_phase;

typedef enum ui_trace_kind {
    UI_TRACE_FRAME_BEGIN = 0, // core starts a frame for a surface
    UI_TRACE_FRAME_END,       // render has presented (or skipped) that frame
    UI_TRACE_PHASE_BEGIN,
    UI_TRACE_PHASE_END,
    UI_TRACE_PRESENTED,       // the compositor reports the frame reached the screen
} ui_trace_kind;

typedef struct ui_trace_event {
    ui_trace_kind kind;
    ui_phase phase;        // phase events only
    ui_handle surface;
    uint64_t frame_id;
    uint64_t time_ns;      // CLOCK_MONOTONIC; for PRESENTED, the reported present time
    uint64_t refresh_ns;   // PRESENTED only; 0 if unknown
} ui_trace_event;

typedef void (*ui_trace_fn)(void *user, const ui_trace_event *event);

// Thread: main, and only while ui_app_run or ui_app_frame is not executing.
// Returns UI_ERR_FULL when UI_HOOKS_MAX_SUBSCRIBERS are registered and UI_ERR_UNSUPPORTED
// when the library was built without hooks.
ui_result ui_hooks_subscribe(ui_app *app, ui_trace_fn fn, void *user, uint32_t *out_id);
void ui_hooks_unsubscribe(ui_app *app, uint32_t id);

#ifdef __cplusplus
}
#endif
