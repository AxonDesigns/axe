// Wayland platform: connection, xdg-shell toplevels, decorations, fractional scale and the
// seat. Frame pacing lives in pacing.c. Everything here runs on the main thread; the render
// thread only calls ui_wl_pre_present.
#include <errno.h>
#include <stdint.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

#include "platform/wayland/wayland_internal.h"

#define MODULE "wayland"
#define LOG(p) ((p)->base.log)

static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

// ---- Configure ---------------------------------------------------------------------------

static void report_config(ui_psurface *s, uint64_t token) {
    s->config.token = token;
    s->platform->base.listener.configure(s->platform->base.listener.user, s->core_tag, &s->config);
}

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
    (void)xdg_surface;
    ui_psurface *s = data;
    s->config.logical_width = s->pending_width ? s->pending_width : s->default_width;
    s->config.logical_height = s->pending_height ? s->pending_height : s->default_height;
    // The serial is acked by the render thread in the commit that carries a buffer of this
    // size (ui_wl_pre_present). Bit 32 keeps the token non-zero even for serial 0.
    report_config(s, (1ull << 32) | serial);
    UI_DEBUG(LOG(s->platform), MODULE, "configure %ux%u @ %u/120 (serial %u)",
             s->config.logical_width, s->config.logical_height, s->config.scale_120, serial);
    if (!s->configured) {
        s->configured = true;
        ui_wl_pacing_first_frame(s);
    }
}

static const struct xdg_surface_listener xdg_surface_listener = {.configure = xdg_surface_configure};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width,
                               int32_t height, struct wl_array *states) {
    (void)toplevel;
    (void)states;
    ui_psurface *s = data;
    // 0 means "client decides": keep the current size.
    if (width > 0) s->pending_width = (uint32_t)width;
    if (height > 0) s->pending_height = (uint32_t)height;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    (void)toplevel;
    ui_psurface *s = data;
    UI_INFO(LOG(s->platform), MODULE, "close requested");
    s->platform->base.listener.close(s->platform->base.listener.user, s->core_tag);
}

static void toplevel_configure_bounds(void *data, struct xdg_toplevel *toplevel, int32_t width,
                                      int32_t height) {
    (void)data, (void)toplevel, (void)width, (void)height;
}

static void toplevel_wm_capabilities(void *data, struct xdg_toplevel *toplevel,
                                     struct wl_array *caps) {
    (void)data, (void)toplevel, (void)caps;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_configure_bounds,
    .wm_capabilities = toplevel_wm_capabilities,
};

static void decoration_configure(void *data, struct zxdg_toplevel_decoration_v1 *decoration,
                                 uint32_t mode) {
    (void)decoration;
    ui_psurface *s = data;
    if (mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE)
        UI_INFO(LOG(s->platform), MODULE,
                "compositor wants client-side decorations; none are drawn yet");
    else
        UI_DEBUG(LOG(s->platform), MODULE, "server-side decorations");
}

static const struct zxdg_toplevel_decoration_v1_listener decoration_listener = {
    .configure = decoration_configure,
};

static void preferred_scale(void *data, struct wp_fractional_scale_v1 *fs, uint32_t scale) {
    (void)fs;
    ui_psurface *s = data;
    if (scale == s->config.scale_120) return;
    UI_INFO(LOG(s->platform), MODULE, "scale %.3f", scale / 120.0);
    s->config.scale_120 = scale;
    // Only the buffer size changes: the logical size, and so the configure serial, stay.
    if (s->configured) report_config(s, s->config.token);
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = preferred_scale,
};

static void surface_enter(void *data, struct wl_surface *surface, struct wl_output *output) {
    (void)data, (void)surface, (void)output;
}

static void surface_leave(void *data, struct wl_surface *surface, struct wl_output *output) {
    (void)data, (void)surface, (void)output;
}

static const struct wl_surface_listener surface_listener = {
    .enter = surface_enter,
    .leave = surface_leave,
};

// ---- Seat: input is only logged in this milestone ---------------------------------------

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    (void)pointer, (void)serial, (void)surface;
    wl_platform *p = data;
    UI_DEBUG(LOG(p), MODULE, "pointer enter %.1f,%.1f", wl_fixed_to_double(x), wl_fixed_to_double(y));
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface) {
    (void)pointer, (void)serial, (void)surface;
    UI_DEBUG(LOG((wl_platform *)data), MODULE, "pointer leave");
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t x,
                           wl_fixed_t y) {
    (void)pointer, (void)time;
    UI_TRACE_LOG(LOG((wl_platform *)data), MODULE, "pointer motion %.1f,%.1f",
                 wl_fixed_to_double(x), wl_fixed_to_double(y));
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t state) {
    (void)pointer, (void)serial, (void)time;
    UI_INFO(LOG((wl_platform *)data), MODULE, "pointer button 0x%x %s", button,
            state == WL_POINTER_BUTTON_STATE_PRESSED ? "down" : "up");
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis,
                         wl_fixed_t value) {
    (void)pointer, (void)time;
    UI_DEBUG(LOG((wl_platform *)data), MODULE, "pointer axis %u %.2f", axis,
             wl_fixed_to_double(value));
}

static void pointer_frame(void *data, struct wl_pointer *pointer) { (void)data, (void)pointer; }

static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source) {
    (void)data, (void)pointer, (void)source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis) {
    (void)data, (void)pointer, (void)time, (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis,
                                  int32_t discrete) {
    (void)data, (void)pointer, (void)axis, (void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd,
                            uint32_t size) {
    (void)keyboard;
    wl_platform *p = data;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }
    // Seat v7+: the keymap must be mapped private.
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        UI_WARN(LOG(p), MODULE, "cannot map keymap");
        return;
    }
    struct xkb_keymap *keymap = xkb_keymap_new_from_string(p->xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1,
                                                           XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (!keymap) {
        UI_WARN(LOG(p), MODULE, "cannot compile keymap");
        return;
    }
    struct xkb_state *state = xkb_state_new(keymap);
    if (!state) {
        xkb_keymap_unref(keymap);
        return;
    }
    xkb_state_unref(p->xkb_state);
    xkb_keymap_unref(p->keymap);
    p->keymap = keymap;
    p->xkb_state = state;
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface, struct wl_array *keys) {
    (void)keyboard, (void)serial, (void)surface, (void)keys;
    UI_DEBUG(LOG((wl_platform *)data), MODULE, "keyboard focus in");
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface) {
    (void)keyboard, (void)serial, (void)surface;
    UI_DEBUG(LOG((wl_platform *)data), MODULE, "keyboard focus out");
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time,
                         uint32_t key, uint32_t state) {
    (void)keyboard, (void)serial, (void)time;
    wl_platform *p = data;
    if (!p->xkb_state) return;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(p->xkb_state, key + 8); // evdev -> xkb
    char name[64];
    xkb_keysym_get_name(sym, name, sizeof name);
    UI_INFO(LOG(p), MODULE, "key %s %s", name,
            state == WL_KEYBOARD_KEY_STATE_PRESSED ? "down" : "up");
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                               uint32_t depressed, uint32_t latched, uint32_t locked,
                               uint32_t group) {
    (void)keyboard, (void)serial;
    wl_platform *p = data;
    if (p->xkb_state) xkb_state_update_mask(p->xkb_state, depressed, latched, locked, 0, 0, group);
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate,
                                 int32_t delay) {
    (void)data, (void)keyboard, (void)rate, (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    wl_platform *p = data;
    bool has_pointer = caps & WL_SEAT_CAPABILITY_POINTER;
    bool has_keyboard = caps & WL_SEAT_CAPABILITY_KEYBOARD;
    if (has_pointer && !p->pointer) {
        p->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(p->pointer, &pointer_listener, p);
    } else if (!has_pointer && p->pointer) {
        wl_pointer_release(p->pointer);
        p->pointer = NULL;
    }
    if (has_keyboard && !p->keyboard) {
        p->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(p->keyboard, &keyboard_listener, p);
    } else if (!has_keyboard && p->keyboard) {
        wl_keyboard_release(p->keyboard);
        p->keyboard = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
    (void)seat;
    UI_DEBUG(LOG((wl_platform *)data), MODULE, "seat %s", name);
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

// ---- Registry ----------------------------------------------------------------------------

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {.ping = wm_base_ping};

static void presentation_clock_id(void *data, struct wp_presentation *presentation, uint32_t clk) {
    (void)presentation;
    ((wl_platform *)data)->presentation_clock = (clockid_t)clk;
}

static const struct wp_presentation_listener presentation_listener = {
    .clock_id = presentation_clock_id,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version) {
    wl_platform *p = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        p->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, min_u32(version, 4));
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        p->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, min_u32(version, 5));
        xdg_wm_base_add_listener(p->wm_base, &wm_base_listener, p);
    } else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
        p->decoration_manager =
            wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        p->fractional_scale_manager =
            wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1);
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        p->viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    } else if (strcmp(interface, wp_presentation_interface.name) == 0) {
        p->presentation = wl_registry_bind(registry, name, &wp_presentation_interface, 1);
        wp_presentation_add_listener(p->presentation, &presentation_listener, p);
    } else if (strcmp(interface, wl_seat_interface.name) == 0 && !p->seat) {
        // v7: private keymap mapping. Newer pointer events (v8+) are not needed yet.
        p->seat = wl_registry_bind(registry, name, &wl_seat_interface, min_u32(version, 7));
        wl_seat_add_listener(p->seat, &seat_listener, p);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data, (void)registry, (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

// ---- Platform vtable ---------------------------------------------------------------------

static void destroy(ui_platform *base) {
    wl_platform *p = (wl_platform *)base;
    if (p->keyboard) wl_keyboard_release(p->keyboard);
    if (p->pointer) wl_pointer_release(p->pointer);
    if (p->seat) wl_seat_release(p->seat);
    if (p->presentation) wp_presentation_destroy(p->presentation);
    if (p->viewporter) wp_viewporter_destroy(p->viewporter);
    if (p->fractional_scale_manager) wp_fractional_scale_manager_v1_destroy(p->fractional_scale_manager);
    if (p->decoration_manager) zxdg_decoration_manager_v1_destroy(p->decoration_manager);
    if (p->wm_base) xdg_wm_base_destroy(p->wm_base);
    if (p->compositor) wl_compositor_destroy(p->compositor);
    if (p->registry) wl_registry_destroy(p->registry);
    xkb_state_unref(p->xkb_state);
    xkb_keymap_unref(p->keymap);
    xkb_context_unref(p->xkb);
    if (p->display) wl_display_disconnect(p->display);
    if (p->wake_fd >= 0) close(p->wake_fd);
    free(p);
}

static void surface_destroy(ui_platform *base, ui_psurface *s) {
    (void)base;
    ui_wl_pacing_destroy(s);
    if (s->viewport) wp_viewport_destroy(s->viewport);
    if (s->fractional_scale) wp_fractional_scale_v1_destroy(s->fractional_scale);
    if (s->decoration) zxdg_toplevel_decoration_v1_destroy(s->decoration);
    if (s->toplevel) xdg_toplevel_destroy(s->toplevel);
    if (s->xdg_surface) xdg_surface_destroy(s->xdg_surface);
    if (s->surface_wrapper) wl_proxy_wrapper_destroy(s->surface_wrapper);
    if (s->surface) wl_surface_destroy(s->surface);
    wl_display_flush(s->platform->display);
    mtx_destroy(&s->pending_lock);
    free(s);
}

static ui_result surface_create(ui_platform *base, const ui_surface_desc *desc, void *core_tag,
                                ui_psurface **out) {
    wl_platform *p = (wl_platform *)base;
    if (desc->kind != UI_SURFACE_WINDOW || !desc->width || !desc->height)
        return UI_ERR_INVALID_ARGUMENT;
    ui_psurface *s = calloc(1, sizeof *s);
    if (!s) return UI_ERR_OUT_OF_MEMORY;
    if (mtx_init(&s->pending_lock, mtx_plain) != thrd_success) {
        free(s);
        return UI_ERR_OUT_OF_MEMORY;
    }
    s->platform = p;
    s->core_tag = core_tag;
    s->default_width = desc->width;
    s->default_height = desc->height;
    s->config.scale_120 = 120;
    s->buffer_scale = 1;

    s->surface = wl_compositor_create_surface(p->compositor);
    wl_surface_add_listener(s->surface, &surface_listener, s);
    s->surface_wrapper = wl_proxy_create_wrapper(s->surface);
    if (!s->surface_wrapper) {
        wl_surface_destroy(s->surface);
        mtx_destroy(&s->pending_lock);
        free(s);
        return UI_ERR_OUT_OF_MEMORY;
    }
    wl_proxy_set_queue((struct wl_proxy *)s->surface_wrapper, NULL); // default queue
    s->xdg_surface = xdg_wm_base_get_xdg_surface(p->wm_base, s->surface);
    xdg_surface_add_listener(s->xdg_surface, &xdg_surface_listener, s);
    s->toplevel = xdg_surface_get_toplevel(s->xdg_surface);
    xdg_toplevel_add_listener(s->toplevel, &toplevel_listener, s);
    xdg_toplevel_set_title(s->toplevel, desc->title ? desc->title : "axe");
    xdg_toplevel_set_app_id(s->toplevel, "dev.axe.app");

    if (p->decoration_manager) {
        s->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(p->decoration_manager,
                                                                           s->toplevel);
        zxdg_toplevel_decoration_v1_add_listener(s->decoration, &decoration_listener, s);
        zxdg_toplevel_decoration_v1_set_mode(s->decoration,
                                             ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    } else {
        UI_INFO(LOG(p), MODULE, "no xdg-decoration: the compositor expects client-side "
                                "decorations; none are drawn yet");
    }
    if (p->fractional_scale_manager && p->viewporter) {
        s->fractional_scale =
            wp_fractional_scale_manager_v1_get_fractional_scale(p->fractional_scale_manager, s->surface);
        wp_fractional_scale_v1_add_listener(s->fractional_scale, &fractional_scale_listener, s);
    } else {
        UI_WARN(LOG(p), MODULE, "no fractional-scale-v1 + viewporter: rendering at scale 1");
    }
    if (p->viewporter) s->viewport = wp_viewporter_get_viewport(p->viewporter, s->surface);

    // Initial commit without a buffer, then wait for the first configure.
    wl_surface_commit(s->surface);
    for (int i = 0; i < 100 && !s->configured; i++) {
        if (wl_display_roundtrip(p->display) < 0) break;
    }
    if (!s->configured) {
        UI_ERROR(LOG(p), MODULE, "no configure from the compositor");
        surface_destroy(base, s);
        return UI_ERR_PLATFORM;
    }
    *out = s;
    return UI_OK;
}

static void surface_native(ui_psurface *s, ui_native_surface *out) {
    *out = (ui_native_surface){
        .kind = UI_NATIVE_WAYLAND, .display = s->platform->display, .surface = s->surface,
    };
}

static ui_result surface_set_position(ui_psurface *s, int32_t x, int32_t y) {
    (void)s, (void)x, (void)y;
    return UI_ERR_UNSUPPORTED; // Wayland clients cannot position their windows
}

static ui_result wait_events(ui_platform *base, uint64_t timeout_ns) {
    wl_platform *p = (wl_platform *)base;
    // prepare_read/read_events cooperates with other threads reading the same display
    // (the Vulkan WSI on the render thread uses its own event queue).
    while (wl_display_prepare_read(p->display) != 0) {
        if (wl_display_dispatch_pending(p->display) < 0) return UI_ERR_PLATFORM;
    }
    if (wl_display_flush(p->display) < 0 && errno != EAGAIN) {
        wl_display_cancel_read(p->display);
        return UI_ERR_PLATFORM;
    }
    struct pollfd fds[2] = {
        {.fd = wl_display_get_fd(p->display), .events = POLLIN},
        {.fd = p->wake_fd, .events = POLLIN},
    };
    // Round up: waking early for a timer would spin until it is due.
    int ms = timeout_ns >= (uint64_t)INT32_MAX * 1000000u ? -1
                                                          : (int)((timeout_ns + 999999u) / 1000000u);
    int n = poll(fds, 2, ms);
    if (n < 0 && errno != EINTR) {
        wl_display_cancel_read(p->display);
        return UI_ERR_PLATFORM;
    }
    if (n > 0 && (fds[0].revents & POLLIN)) {
        if (wl_display_read_events(p->display) < 0) return UI_ERR_PLATFORM;
    } else {
        wl_display_cancel_read(p->display);
    }
    if (fds[0].revents & (POLLERR | POLLHUP)) {
        UI_ERROR(LOG(p), MODULE, "compositor connection lost");
        return UI_ERR_PLATFORM;
    }
    if (fds[1].revents & POLLIN) {
        uint64_t drain;
        (void)!read(p->wake_fd, &drain, sizeof drain);
    }
    return wl_display_dispatch_pending(p->display) < 0 ? UI_ERR_PLATFORM : UI_OK;
}

static void wake(ui_platform *base) {
    wl_platform *p = (wl_platform *)base;
    uint64_t one = 1;
    (void)!write(p->wake_fd, &one, sizeof one);
}

static const ui_platform_vtable vtable = {
    .destroy = destroy,
    .surface_create = surface_create,
    .surface_destroy = surface_destroy,
    .surface_native = surface_native,
    .surface_pre_present = ui_wl_pre_present,
    .surface_set_position = surface_set_position,
    .surface_request_frame = ui_wl_request_frame,
    .surface_period_ns = ui_wl_period_ns,
    .wait = wait_events,
    .wake = wake,
};

ui_result ui_platform_wayland_create(const ui_logger *log, ui_platform_listener listener,
                                     ui_platform **out) {
    wl_platform *p = calloc(1, sizeof *p);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p->base = (ui_platform){.vt = &vtable, .listener = listener, .log = log};
    p->presentation_clock = CLOCK_MONOTONIC;
    p->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    p->display = wl_display_connect(NULL);
    if (!p->display || p->wake_fd < 0) {
        UI_ERROR(log, MODULE, "cannot connect to a Wayland compositor (WAYLAND_DISPLAY=%s)",
                 getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "unset");
        destroy(&p->base);
        return UI_ERR_PLATFORM;
    }
    p->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    p->registry = wl_display_get_registry(p->display);
    wl_registry_add_listener(p->registry, &registry_listener, p);
    // Globals, then the events of what they bound (seat capabilities, presentation clock).
    if (wl_display_roundtrip(p->display) < 0 || wl_display_roundtrip(p->display) < 0) {
        destroy(&p->base);
        return UI_ERR_PLATFORM;
    }
    if (!p->compositor || !p->wm_base) {
        UI_ERROR(log, MODULE, "compositor lacks wl_compositor or xdg_wm_base");
        destroy(&p->base);
        return UI_ERR_UNSUPPORTED;
    }
    UI_INFO(log, MODULE, "globals: decoration %s, fractional-scale %s, viewporter %s, "
                         "presentation-time %s",
            p->decoration_manager ? "yes" : "no", p->fractional_scale_manager ? "yes" : "no",
            p->viewporter ? "yes" : "no", p->presentation ? "yes" : "no");
    *out = &p->base;
    return UI_OK;
}
