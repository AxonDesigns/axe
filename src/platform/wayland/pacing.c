// Frame pacing on Wayland. The render thread attaches per-commit state right before it
// presents (configure ack, viewport, frame callback, presentation feedback); the
// compositor's replies are dispatched on the main thread and turned into "draw the next
// frame, which will reach the screen at time T" for the core.
//
// Prediction: presentation feedback gives the last present time and the refresh period.
// A frame drawn now is expected at the first vblank after now, plus `frames_ahead` periods,
// which adapts when feedback shows presents landing consistently a period late or early.
#include <stdlib.h>

#include "platform/wayland/wayland_internal.h"

#define MODULE "wayland"
#define DEFAULT_REFRESH_NS 16666667u
#define MAX_FRAMES_AHEAD 3
#define ADAPT_STREAK 4

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

static uint64_t period(const ui_psurface *s) {
    if (s->refresh_ns) return s->refresh_ns;
    if (s->measured_period_ns > 0) return (uint64_t)s->measured_period_ns;
    return DEFAULT_REFRESH_NS;
}

static uint64_t predict_present(const ui_psurface *s, uint64_t now) {
    uint64_t refresh = period(s);
    uint64_t next;
    if (s->last_present_ns && now >= s->last_present_ns)
        next = s->last_present_ns + refresh * ((now - s->last_present_ns) / refresh + 1);
    else
        next = now + refresh;
    return next + (uint64_t)s->frames_ahead * refresh;
}

// Main thread.
static void frame_done(void *data, struct wl_callback *callback, uint32_t time_ms) {
    (void)time_ms;
    wl_pending *p = data;
    ui_psurface *s = p->surface;
    pending_remove(s, p);
    wl_callback_destroy(callback);
    free(p);
    report_frame(s, predict_present(s, ui_wl_now_ns()));
}

static const struct wl_callback_listener frame_listener = {.done = frame_done};

void ui_wl_pacing_first_frame(ui_psurface *s) { report_frame(s, predict_present(s, ui_wl_now_ns())); }

// Converts a timestamp of the compositor's presentation clock to CLOCK_MONOTONIC.
static uint64_t to_monotonic(clockid_t clock, uint64_t t) {
    if (clock == CLOCK_MONOTONIC) return t;
    struct timespec ts;
    clock_gettime(clock, &ts);
    uint64_t other = (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
    uint64_t mono = ui_wl_now_ns();
    return t + mono - other;
}

static void adapt(ui_psurface *s, int64_t error_ns) {
    int64_t half = (int64_t)period(s) / 2;
    if (error_ns > half) {
        s->early_streak = 0;
        if (++s->late_streak >= ADAPT_STREAK && s->frames_ahead < MAX_FRAMES_AHEAD) {
            s->frames_ahead++;
            s->late_streak = 0;
        }
    } else if (error_ns < -half) {
        s->late_streak = 0;
        if (++s->early_streak >= ADAPT_STREAK && s->frames_ahead > 0) {
            s->frames_ahead--;
            s->early_streak = 0;
        }
    } else {
        s->late_streak = s->early_streak = 0;
    }
}

static void feedback_finish(wl_pending *p, struct wp_presentation_feedback *feedback,
                            uint64_t present_ns, uint64_t refresh_ns) {
    ui_psurface *s = p->surface;
    pending_remove(s, p);
    wp_presentation_feedback_destroy(feedback);
    s->platform->base.listener.presented(s->platform->base.listener.user, s->core_tag,
                                         p->frame_id, present_ns, refresh_ns);
    free(p);
}

static void feedback_sync_output(void *data, struct wp_presentation_feedback *feedback,
                                 struct wl_output *output) {
    (void)data, (void)feedback, (void)output;
}

static void feedback_presented(void *data, struct wp_presentation_feedback *feedback,
                               uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec,
                               uint32_t refresh, uint32_t seq_hi, uint32_t seq_lo,
                               uint32_t flags) {
    (void)seq_hi, (void)seq_lo, (void)flags;
    wl_pending *p = data;
    ui_psurface *s = p->surface;
    uint64_t t = (((uint64_t)tv_sec_hi << 32) | tv_sec_lo) * 1000000000u + tv_nsec;
    t = to_monotonic(s->platform->presentation_clock, t);
    if (s->last_present_ns && t > s->last_present_ns) {
        // Smoothed present interval; gaps (missed frames, idle) are not periods.
        double d = (double)(t - s->last_present_ns);
        if (s->measured_period_ns <= 0) s->measured_period_ns = d;
        else if (d < s->measured_period_ns * 1.5) s->measured_period_ns += (d - s->measured_period_ns) * 0.1;
    }
    if (t > s->last_present_ns) s->last_present_ns = t;
    s->refresh_ns = refresh; // 0: unknown or variable refresh, use the measured period
    adapt(s, (int64_t)(t - p->predicted_ns));
    UI_TRACE_LOG(s->platform->base.log, MODULE,
                 "frame %llu presented %+.2f ms vs prediction (refresh %.2f ms, ahead %d)",
                 (unsigned long long)p->frame_id, (double)(int64_t)(t - p->predicted_ns) / 1e6,
                 refresh / 1e6, s->frames_ahead);
    feedback_finish(p, feedback, t, period(s));
}

static void feedback_discarded(void *data, struct wp_presentation_feedback *feedback) {
    feedback_finish(data, feedback, 0, 0);
}

static const struct wp_presentation_feedback_listener feedback_listener = {
    .sync_output = feedback_sync_output,
    .presented = feedback_presented,
    .discarded = feedback_discarded,
};

// Render thread. Feedback proxies inherit the wp_presentation's (default) queue.
static void wl_presentation_feedback_request(ui_psurface *s, const ui_present_info *info) {
    if (!s->platform->presentation) return;
    wl_pending *fb = calloc(1, sizeof *fb);
    if (!fb) return;
    fb->surface = s;
    fb->frame_id = info->frame_id;
    fb->predicted_ns = info->predicted_present_host_ns;
    fb->feedback = wp_presentation_feedback(s->platform->presentation, s->surface_wrapper);
    wp_presentation_feedback_add_listener(fb->feedback, &feedback_listener, fb);
    pending_add(s, fb);
}

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
    wl_presentation_feedback_request(s, info);
}

// Main thread. A surface that went idle has no frame in flight and no callback pending.
// Request one and commit with no new buffer: the compositor answers at its next repaint,
// so the frame that follows is paced exactly like the ones before the idle period.
void ui_wl_request_frame(ui_psurface *s) {
    wl_pending *frame = calloc(1, sizeof *frame);
    if (!frame) {
        // Without a callback we would never draw again; draw on the platform default.
        report_frame(s, predict_present(s, ui_wl_now_ns()));
        return;
    }
    frame->surface = s;
    frame->frame = wl_surface_frame(s->surface_wrapper);
    wl_callback_add_listener(frame->frame, &frame_listener, frame);
    pending_add(s, frame);
    wl_surface_commit(s->surface);
}

uint64_t ui_wl_period_ns(ui_psurface *s) { return period(s); }

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
