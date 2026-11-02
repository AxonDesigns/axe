// Internal platform interface. One implementation per windowing system plus headless.
// Platform objects are created, dispatched and destroyed on the main (core) thread; the only
// exceptions are wake() (any thread) and surface_pre_present() (render actor).
#pragma once

#include <stdint.h>

#include "runtime/common.h"
#include "ui/app.h"

typedef struct ui_platform ui_platform;
typedef struct ui_psurface ui_psurface;

typedef struct ui_surface_config {
    uint32_t logical_width, logical_height;
    uint32_t scale_120;
    uint64_t token; // opaque; echoed back to pre_present with lists rendered at this size
} ui_surface_config;

// Callbacks into the core. They run on the main thread from inside wait() (or surface
// creation) and receive the `core_tag` given at surface creation.
typedef struct ui_platform_listener {
    void *user;
    void (*configure)(void *user, void *core_tag, const ui_surface_config *config);
    // The surface may draw its next frame, expected on screen at the given host time.
    void (*frame)(void *user, void *core_tag, uint64_t predicted_present_host_ns);
    void (*close)(void *user, void *core_tag);
    // Feedback for a presented frame (present_ns 0: discarded by the compositor). The
    // refresh period is the compositor's, or the measured one when it reports none (VRR).
    void (*presented)(void *user, void *core_tag, uint64_t frame_id, uint64_t present_ns,
                      uint64_t refresh_ns);
} ui_platform_listener;

typedef enum ui_native_kind {
    UI_NATIVE_OFFSCREEN = 0,
    UI_NATIVE_WAYLAND,
} ui_native_kind;

// Handed to the renderer to create its GPU surface. No platform types; just pointers.
typedef struct ui_native_surface {
    ui_native_kind kind;
    void *display; // wl_display*
    void *surface; // wl_surface*
} ui_native_surface;

typedef struct ui_present_info {
    uint64_t token;
    uint64_t frame_id;
    uint64_t predicted_present_host_ns; // what the core expected; checked against feedback
    float logical_width, logical_height;
    uint32_t scale_120;
} ui_present_info;

typedef struct ui_platform_vtable {
    void (*destroy)(ui_platform *p);
    ui_result (*surface_create)(ui_platform *p, const ui_surface_desc *desc, void *core_tag,
                                ui_psurface **out);
    void (*surface_destroy)(ui_platform *p, ui_psurface *s);
    void (*surface_native)(ui_psurface *s, ui_native_surface *out);
    // Render actor, right before presenting a frame rendered for `info->token`. Attaches
    // per-commit state (configure ack, viewport, frame and presentation callbacks) so it
    // lands in the same surface commit as the frame's buffer.
    void (*surface_pre_present)(ui_psurface *s, const ui_present_info *info);
    ui_result (*surface_set_position)(ui_psurface *s, int32_t x, int32_t y);
    // Main thread. Asks for one frame callback on a surface that stopped drawing because it
    // went idle, without presenting a new buffer, so the next frame is still paced by the
    // compositor. Only called while no frame of this surface is in flight.
    void (*surface_request_frame)(ui_psurface *s);
    // Main thread. The surface's current refresh period: reported by the compositor, else
    // measured from presents, else the platform default.
    uint64_t (*surface_period_ns)(ui_psurface *s);
    // Blocks until platform events or wake(), at most `timeout_ns` (UINT64_MAX: forever),
    // then dispatches events to the listener.
    ui_result (*wait)(ui_platform *p, uint64_t timeout_ns);
    void (*wake)(ui_platform *p);
} ui_platform_vtable;

struct ui_platform {
    const ui_platform_vtable *vt;
    ui_platform_listener listener;
    const ui_logger *log;
};

// Thread: main. Returns UI_ERR_PLATFORM when no compositor is reachable.
ui_result ui_platform_wayland_create(const ui_logger *log, ui_platform_listener listener,
                                     ui_platform **out);
ui_result ui_platform_headless_create(const ui_logger *log, ui_platform_listener listener,
                                      ui_platform **out);
