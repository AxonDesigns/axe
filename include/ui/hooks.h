// Hook API v0: tracepoints for frames and frame phases.
//
// Tracepoints cost nothing when the library is built with UI_HOOKS=OFF, and one relaxed
// atomic load when built in but nobody subscribes. Callbacks run synchronously on the
// thread that hits the tracepoint (core, render or a worker), so they must be short and
// thread-safe.
#pragma once

#include <stdint.h>

#include "ui/handle.h"
#include "ui/result.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_HOOKS_VERSION 4u // 1: script events, `text` and `duration_ns` (milestone 3a); 2: COMMANDS_APPLIED (3b); 3: SCRIPT_HOT_UPDATE (3b.3); 4: DEVICE_LOST and the phases of single GPU calls
#define UI_HOOKS_MAX_SUBSCRIBERS 8u

typedef struct ui_app ui_app;

typedef enum ui_phase {
    UI_PHASE_APPLY = 0, // core: apply command buffers
    UI_PHASE_ANIMATE,   // core: tick springs
    UI_PHASE_COMMIT,    // core: build and publish display lists
    UI_PHASE_ENCODE,    // render: acquire the target, record and submit GPU work
    UI_PHASE_PRESENT,   // render: per-commit platform state and the present call
    // Since version 4: single GPU calls, nested in the two phases above, so don't add them to
    // those. ACQUIRE and SUBMIT are inside ENCODE, and SWAP inside PRESENT.
    UI_PHASE_ACQUIRE,   // render: wgpuSurfaceGetCurrentTexture, or preparing an offscreen target
    UI_PHASE_SUBMIT,    // render: wgpuQueueSubmit
    UI_PHASE_SWAP,      // render: wgpuSurfacePresent
    UI_PHASE_COUNT,
} ui_phase;

typedef enum ui_trace_kind {
    UI_TRACE_FRAME_BEGIN = 0, // core starts a frame for a surface
    UI_TRACE_FRAME_END,       // render has presented (or skipped) that frame
    UI_TRACE_PHASE_BEGIN,
    UI_TRACE_PHASE_END,
    UI_TRACE_PRESENTED,       // the compositor reports the frame reached the screen
    UI_TRACE_FRAME_IDLE,      // a frame opportunity had nothing to draw: the surface idles
    // A worker starts or finishes a job preparing a command buffer: decoding and shaping one
    // set_text, rasterizing a batch of its glyphs, reading a font file, or shaping one
    // paragraph again after a font load. frame_id carries the buffer's sequence number,
    // counted from 1 in the order prepared buffers arrive; surface is null.
    UI_TRACE_PREPARE_BEGIN,
    UI_TRACE_PREPARE_END,
    // The script runtime (docs/script.md, "Errors"), on the logic thread. `text` describes
    // what happened (with a stack trace for errors); surface is null.
    UI_TRACE_SCRIPT_EXCEPTION, // an uncaught exception: the app keeps running
    UI_TRACE_SCRIPT_REJECTION, // a promise rejected with no handler at a microtask checkpoint
    UI_TRACE_SCRIPT_LONG_TASK, // a task ran longer than the threshold; duration_ns says how long
    UI_TRACE_SCRIPT_LEAK,      // debug builds: a subtree became unreachable without destroy()
    // Since version 2: a command buffer applied in frame `frame_id` (of the app's first surface,
    // `surface`); `time_ns` is when the logic thread sent it. With PRESENTED for that frame, the
    // time from a turn's writes to the screen.
    UI_TRACE_COMMANDS_APPLIED,
    // Since version 3, in development builds: a turn applied a hot update and flushed it;
    // `duration_ns` is the time since the file event behind it (0: the first build).
    UI_TRACE_SCRIPT_HOT_UPDATE,
    // Since version 4: the GPU device is lost, once per device, on the render thread. Nothing
    // draws from here on, and the app keeps running (docs/architecture.md, "Rendering"). The
    // surface is null.
    UI_TRACE_DEVICE_LOST,
} ui_trace_kind;

typedef struct ui_trace_event {
    ui_trace_kind kind;
    ui_phase phase;        // phase events only
    ui_handle surface;
    uint64_t frame_id;
    uint64_t time_ns;      // CLOCK_MONOTONIC; for PRESENTED, the reported present time
    uint64_t refresh_ns;   // PRESENTED only; 0 if unknown
    // Since version 1: script events only (NULL otherwise). Valid during the callback.
    const char *text;
    uint64_t duration_ns;  // SCRIPT_LONG_TASK and SCRIPT_HOT_UPDATE only
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
