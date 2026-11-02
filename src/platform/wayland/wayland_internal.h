// Shared state of the Wayland platform. Each field notes which thread owns it.
#pragma once

#include <threads.h>
#include <time.h>
#include <wayland-client.h>

#include "fractional-scale-v1-client-protocol.h"
#include "platform/platform.h"
#include "presentation-time-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

struct xkb_context;
struct xkb_keymap;
struct xkb_state;

typedef struct wl_platform {
    ui_platform base;
    // Main thread, except that requests on these proxies are thread-safe in libwayland.
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct xdg_wm_base *wm_base;
    struct zxdg_decoration_manager_v1 *decoration_manager;
    struct wp_fractional_scale_manager_v1 *fractional_scale_manager;
    struct wp_viewporter *viewporter;
    struct wp_presentation *presentation;
    clockid_t presentation_clock;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct xkb_context *xkb;
    struct xkb_keymap *keymap;
    struct xkb_state *xkb_state;
    int wake_fd;
} wl_platform;

// A request the render thread made whose reply arrives on the main thread. Tracked so
// surface destruction can destroy proxies whose events never came.
typedef struct wl_pending {
    struct wl_pending *next;
    struct ui_psurface *surface;
    struct wl_callback *frame;                      // or
    struct wp_presentation_feedback *feedback;
    uint64_t frame_id;
    uint64_t predicted_ns;
} wl_pending;

struct ui_psurface {
    wl_platform *platform;
    void *core_tag;
    struct wl_surface *surface;
    // Same object, pinned to the default queue: callbacks created through it are dispatched
    // on the main thread even if the Vulkan WSI moves `surface` to a private queue.
    struct wl_surface *surface_wrapper;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct zxdg_toplevel_decoration_v1 *decoration;
    struct wp_fractional_scale_v1 *fractional_scale;
    struct wp_viewport *viewport;

    // Main thread: configure state.
    uint32_t default_width, default_height;
    uint32_t pending_width, pending_height; // from xdg_toplevel.configure
    ui_surface_config config;              // last config reported to the core
    bool configured;

    // Main thread: pacing.
    uint64_t last_present_ns; // CLOCK_MONOTONIC
    uint64_t refresh_ns;      // 0 until known
    int frames_ahead;         // presents land this many refreshes after the next vblank
    int late_streak, early_streak;

    // Render thread only: per-commit state already sent.
    uint64_t acked_token;
    float viewport_width, viewport_height;
    int32_t buffer_scale;

    // Shared between render (adds) and main (removes): outstanding callbacks.
    mtx_t pending_lock;
    wl_pending *pending;
};

// pacing.c
void ui_wl_pre_present(ui_psurface *s, const ui_present_info *info);
void ui_wl_pacing_first_frame(ui_psurface *s);
void ui_wl_pacing_destroy(ui_psurface *s);
uint64_t ui_wl_now_ns(void);
