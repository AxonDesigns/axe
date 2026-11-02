// Frame pacing on Wayland. The render thread attaches per-commit state right before it
// presents (configure ack, viewport, frame callback); the compositor's replies are
// dispatched on the main thread and turned into "draw the next frame" for the core.
#include <stdlib.h>

#include "platform/wayland/wayland_internal.h"

#define MODULE "wayland"
#define DEFAULT_REFRESH_NS 16666667u

uint64_t ui_wl_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static void pending_add(ui_psurface *s, wl_pending *p) {
    mtx_lock(&s->pending_lock);
    p->next = s->pending;
    s->pending = p;
    mtx_unlock(&s->pending_lock);
}

static void pending_remove(ui_psurface *s, wl_pending *p) {
    mtx_lock(&s->pending_lock);
    for (wl_pending **it = &s->pending; *it; it = &(*it)->next) {
        if (*it == p) {
            *it = p->next;
            break;
        }
    }
    mtx_unlock(&s->pending_lock);
}

static void report_frame(ui_psurface *s, uint64_t predicted_ns) {
    s->platform->base.listener.frame(s->platform->base.listener.user, s->core_tag, predicted_ns);
}

// Main thread.
static void frame_done(void *data, struct wl_callback *callback, uint32_t time_ms) {
    (void)time_ms;
    wl_pending *p = data;
    ui_psurface *s = p->surface;
    pending_remove(s, p);
    UI_TRACE_LOG(s->platform->base.log, MODULE, "frame callback for frame %llu",
                 (unsigned long long)p->frame_id);
    wl_callback_destroy(callback);
    free(p);
    // The frame drawn now reaches the screen at the next refresh.
    uint64_t refresh = s->refresh_ns ? s->refresh_ns : DEFAULT_REFRESH_NS;
    report_frame(s, ui_wl_now_ns() + refresh);
}

static const struct wl_callback_listener frame_listener = {.done = frame_done};

void ui_wl_pacing_first_frame(ui_psurface *s) { report_frame(s, ui_wl_now_ns()); }

// Render thread. libwayland requests are thread-safe; the new callback proxies inherit the
// default queue, so their events are dispatched on the main thread. Their listeners are set
// before the commit that can trigger them, so no event is lost.
void ui_wl_pre_present(ui_psurface *s, const ui_present_info *info) {
    if (info->token != s->acked_token) {
        xdg_surface_ack_configure(s->xdg_surface, (uint32_t)info->token);
        s->acked_token = info->token;
    }
    if (s->viewport) {
        if (info->logical_width != s->viewport_width || info->logical_height != s->viewport_height) {
            wp_viewport_set_destination(s->viewport, (int32_t)info->logical_width,
                                        (int32_t)info->logical_height);
            s->viewport_width = info->logical_width;
            s->viewport_height = info->logical_height;
        }
    } else {
        int32_t scale = (int32_t)((info->scale_120 + 60) / 120);
        if (scale < 1) scale = 1;
        if (scale != s->buffer_scale) {
            wl_surface_set_buffer_scale(s->surface, scale);
            s->buffer_scale = scale;
        }
    }
    wl_pending *frame = calloc(1, sizeof *frame);
    if (frame) {
        frame->surface = s;
        frame->frame_id = info->frame_id;
        frame->frame = wl_surface_frame(s->surface_wrapper);
        wl_callback_add_listener(frame->frame, &frame_listener, frame);
        pending_add(s, frame);
    }
    UI_TRACE_LOG(s->platform->base.log, MODULE, "pre-present frame %llu",
                 (unsigned long long)info->frame_id);
}

// Main thread, after the render thread has stopped.
void ui_wl_pacing_destroy(ui_psurface *s) {
    wl_pending *p = s->pending;
    while (p) {
        wl_pending *next = p->next;
        if (p->frame) wl_callback_destroy(p->frame);
        if (p->feedback) wp_presentation_feedback_destroy(p->feedback);
        free(p);
        p = next;
    }
    s->pending = NULL;
}
