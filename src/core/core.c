#include "core/core.h"

#include <string.h>

#include "render/render.h"
#include "wire/wire.h"

#define MODULE "core"

// Background behind every surface's content: a dark neutral gray, linear.
static const float clear_color[4] = {0.012f, 0.013f, 0.016f, 1.0f};

// At most this many event buffers wait in logic's queue; later events coalesce into one
// buffer, so a stalled logic actor receives one batch instead of a backlog.
#define MAX_EVENTS_IN_FLIGHT 2u

static void core_wake(void *user) {
    ui_core *c = user;
    c->platform->vt->wake(c->platform);
    ui_signal_notify(c->progress);
}

// ---- Timers (animation clock) ----------------------------------------------------------

// Scene callback for set_timer ops, applied at the frame boundary like every command.
static void set_timer(void *user, uint32_t id, uint64_t deadline_ns) {
    ui_core *c = user;
    for (uint32_t i = 0; i < c->timer_count; i++) {
        if (c->timers[i].id != id) continue;
        if (deadline_ns == UI_WIRE_TIMER_CANCEL) c->timers[i] = c->timers[--c->timer_count];
        else c->timers[i].deadline_ns = deadline_ns;
        return;
    }
    if (deadline_ns == UI_WIRE_TIMER_CANCEL) return;
    if (c->timer_count == UI_MAX_TIMERS ||
        ui_grow((void **)&c->timers, &c->timer_capacity, c->timer_count + 1, sizeof *c->timers) !=
            UI_OK) {
        c->scene.stats.dropped++;
        UI_DEBUG_REPORT(c->log, MODULE, "dropped set_timer %u: too many timers", id);
        return;
    }
    c->timers[c->timer_count++] = (ui_core_timer){id, deadline_ns};
}

static int timer_order(const void *a, const void *b) {
    const ui_core_timer *x = a, *y = b;
    if (x->deadline_ns != y->deadline_ns) return x->deadline_ns < y->deadline_ns ? -1 : 1;
    return x->id < y->id ? -1 : x->id > y->id;
}

// Emits timer_fired for every due timer, earliest first (ties by id), and disarms them.
static void fire_timers(ui_core *c) {
    if (!c->timer_count) return;
    uint64_t now = ui_clock_at(&c->clock, ui_clock_host_now(&c->clock));
    qsort(c->timers, c->timer_count, sizeof *c->timers, timer_order);
    uint32_t due = 0;
    while (due < c->timer_count && c->timers[due].deadline_ns <= now) {
        if (ui_wire_event_timer_fired(&c->events, c->timers[due].id, c->timers[due].deadline_ns,
                                      now) != UI_OK) {
            UI_ERROR(c->log, MODULE, "out of memory queueing timer events");
            break;
        }
        due++;
    }
    memmove(c->timers, c->timers + due, (c->timer_count - due) * sizeof *c->timers);
    c->timer_count -= due;
}

ui_result ui_core_init(ui_core *c, const ui_core_desc *desc) {
    memset(c, 0, sizeof *c);
    c->log = desc->log;
    c->hooks = desc->hooks;
    c->platform = desc->platform;
    c->progress = desc->progress;
    ui_clock_init(&c->clock, desc->time, desc->start_paused);
    ui_scene_init(&c->scene, desc->log);
    ui_handle_table_init(&c->surface_handles, UI_HANDLE_SURFACE);
    ui_wire_buf_init(&c->events, UI_WIRE_MAGIC_EVENT);
    c->scene.timer_fn = set_timer;
    c->scene.timer_user = c;
    ui_result r = ui_actor_init(&c->actor, "core", c, NULL, ui_core_step, NULL, 1024,
                                (ui_waker){core_wake, c});
    if (r != UI_OK) return r;
    r = ui_queue_init_unbounded(&c->from_logic, 256, (ui_waker){core_wake, c});
    if (r != UI_OK) ui_actor_destroy(&c->actor);
    return r;
}

void ui_core_destroy(ui_core *c) {
    ui_queue_destroy(&c->from_logic, ui_msg_drop);
    ui_actor_destroy(&c->actor);
    free(c->timers);
    for (uint32_t i = 0; i < c->pending_count; i++) free(c->pending[i].data);
    free(c->pending);
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        for (int j = 0; j < 3; j++) ui_display_list_free(&c->surfaces[i].lists[j]);
    }
    ui_wire_buf_free(&c->events);
    ui_handle_table_destroy(&c->surface_handles);
    ui_scene_destroy(&c->scene);
}

ui_core_surface *ui_core_surface_get(ui_core *c, ui_handle handle) {
    if (!ui_handle_table_valid(&c->surface_handles, handle)) return NULL;
    uint32_t slot = ui_handle_slot(handle);
    return slot < UI_MAX_SURFACES ? &c->surfaces[slot] : NULL;
}

// ---- Platform callbacks (main thread) ---------------------------------------------------

static void on_configure(void *user, void *tag, const ui_surface_config *config) {
    (void)user;
    ui_core_surface *s = tag;
    bool changed = !s->configured || s->config.logical_width != config->logical_width ||
                   s->config.logical_height != config->logical_height ||
                   s->config.scale_120 != config->scale_120;
    // Every new configure is acked by the next drawn frame (ui_wl_pre_present), so a
    // configure that changes only window state still needs one drawn frame.
    if (!s->configured || config->token != s->config.token) s->config_changed = true;
    s->config = *config;
    s->configured = true;
    if (changed) s->resized_pending = true;
}

static void on_frame(void *user, void *tag, uint64_t predicted_present_host_ns) {
    (void)user;
    ui_core_surface *s = tag;
    s->frame_ready = true;
    s->awaiting_frame = false;
    s->predicted_host_ns = predicted_present_host_ns;
}

static void on_close(void *user, void *tag) {
    ui_core *c = user;
    ui_core_surface *s = tag;
    if (ui_wire_event_surface_close(&c->events, s->handle) != UI_OK)
        UI_ERROR(c->log, MODULE, "out of memory queueing close event");
    // v0 policy: closing the window ends the app; logic still sees the event first.
    c->quit_requested = true;
}

static void on_presented(void *user, void *tag, uint64_t frame_id, uint64_t present_ns,
                         uint64_t refresh_ns) {
    ui_core *c = user;
    ui_core_surface *s = tag;
    (void)c, (void)s, (void)frame_id, (void)present_ns, (void)refresh_ns; // hooks may be off
    UI_HOOK_PRESENTED(c->hooks, s->handle, frame_id, present_ns, refresh_ns);
}

ui_platform_listener ui_core_platform_listener(ui_core *c) {
    return (ui_platform_listener){
        .user = c,
        .configure = on_configure,
        .frame = on_frame,
        .close = on_close,
        .presented = on_presented,
    };
}

// ---- Surfaces ----------------------------------------------------------------------------

ui_result ui_core_add_surface(ui_core *c, const ui_surface_desc *desc, ui_handle *out) {
    ui_handle h;
    ui_result r = ui_handle_table_alloc(&c->surface_handles, &h);
    if (r != UI_OK) return r;
    uint32_t slot = ui_handle_slot(h);
    if (slot >= UI_MAX_SURFACES) {
        ui_handle_table_release(&c->surface_handles, h);
        return UI_ERR_FULL;
    }
    ui_core_surface *s = &c->surfaces[slot];
    memset(s, 0, sizeof *s);
    s->core = c;
    s->handle = h;
    s->kind = desc->kind;
    ui_mailbox_init(&s->mailbox, &s->lists[0], &s->lists[1], &s->lists[2]);

    r = ui_scene_add_root(&c->scene, h);
    if (r == UI_OK) r = c->platform->vt->surface_create(c->platform, desc, s, &s->platform);
    if (r != UI_OK) goto fail;
    if (!s->configured) {
        r = UI_ERR_PLATFORM;
        goto fail;
    }

    ui_render_surface_desc *rd = malloc(sizeof *rd);
    if (!rd) {
        r = UI_ERR_OUT_OF_MEMORY;
        goto fail;
    }
    *rd = (ui_render_surface_desc){
        .surface = h, .mailbox = &s->mailbox, .platform = c->platform, .psurface = s->platform,
    };
    c->platform->vt->surface_native(s->platform, &rd->native);
    ui_msg msg = {.type = UI_MSG_SURFACE_ADD, .surface = h, .ptr = rd};
    r = ui_queue_push(&c->render->queue, &msg);
    if (r != UI_OK) {
        free(rd);
        goto fail;
    }
    *out = h;
    return UI_OK;

fail:
    if (s->platform) c->platform->vt->surface_destroy(c->platform, s->platform);
    ui_scene_remove_root(&c->scene, h);
    ui_handle_table_release(&c->surface_handles, h);
    memset(s, 0, sizeof *s);
    return r;
}

void ui_core_remove_surfaces(ui_core *c) {
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        ui_core_surface *s = &c->surfaces[i];
        if (ui_handle_is_null(s->handle)) continue;
        ui_scene_remove_root(&c->scene, s->handle);
        if (s->platform) c->platform->vt->surface_destroy(c->platform, s->platform);
        s->platform = NULL;
        ui_handle_table_release(&c->surface_handles, s->handle);
        s->handle = (ui_handle){0};
    }
}

void ui_core_force_frame(ui_core *c) {
    uint64_t now = ui_clock_host_now(&c->clock);
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        ui_core_surface *s = &c->surfaces[i];
        if (ui_handle_is_null(s->handle) || !s->configured) continue;
        s->frame_ready = true;
        s->predicted_host_ns = now;
    }
}

// ---- Frame -------------------------------------------------------------------------------

static void handle(ui_core *c, ui_msg msg) {
    switch (msg.type) {
    case UI_MSG_CMD_BUFFER:
        // drain() reserved the slot, so a buffer is never refused here.
        c->pending[c->pending_count++] = (struct ui_pending_cmds){msg.ptr, msg.size};
        break;
    case UI_MSG_EVENTS_DONE:
        c->events_in_flight -= (uint32_t)msg.u64;
        break;
    case UI_MSG_FRAME_DONE: {
        ui_core_surface *s = ui_core_surface_get(c, msg.surface);
        if (s && msg.u64 > s->last_done_frame) s->last_done_frame = msg.u64;
        // A skipped frame was never committed, so no frame callback is coming; redraw at the
        // surface's next refresh instead of stalling.
        if (s && msg.size) {
            s->frame_ready = true;
            s->awaiting_frame = false;
            s->force_draw = true;
            s->predicted_host_ns = ui_clock_host_now(&c->clock) +
                                   c->platform->vt->surface_period_ns(s->platform);
        }
        break;
    }
    case UI_MSG_RENDER_FAILED:
        UI_ERROR(c->log, MODULE, "render failed: %s", ui_result_str((ui_result)msg.u64));
        c->failure = (ui_result)msg.u64;
        c->quit_requested = true;
        break;
    default:
        ui_msg_drop(&msg);
        break;
    }
}

static void drain(ui_core *c) {
    ui_msg msg;
    // Reserve room for every queued command buffer before taking any, so the logic channel
    // stays lossless and ordered even when memory is short: on failure the messages simply
    // stay queued until the next step.
    uint32_t queued = ui_queue_count(&c->from_logic);
    if (queued && ui_grow((void **)&c->pending, &c->pending_capacity, c->pending_count + queued,
                          sizeof *c->pending) != UI_OK) {
        UI_ERROR(c->log, MODULE, "out of memory; %u logic message(s) wait for the next step",
                 queued);
        queued = 0;
    }
    while (queued-- && ui_queue_pop(&c->from_logic, &msg)) handle(c, msg);
    while (ui_queue_pop(&c->actor.queue, &msg)) handle(c, msg);
}

static uint32_t physical(uint32_t logical, uint32_t scale_120) {
    // Round half away from zero, as wp_fractional_scale_v1 specifies.
    return (uint32_t)(((uint64_t)logical * scale_120 + 60) / 120);
}

static bool surface_has_work(ui_core *c, const ui_core_surface *s) {
    const ui_scene_root *root = ui_scene_root_of(&c->scene, s->handle);
    return s->force_draw || s->resized_pending || s->config_changed ||
           (root && (root->dirty || root->animating));
}

static void commit(ui_core *c, ui_core_surface *s, uint64_t now) {
    ui_scene_root *root = ui_scene_root_of(&c->scene, s->handle);
    if (root) root->dirty = false;
    s->force_draw = false;
    s->config_changed = false;
    // The drawn frame's presentation requests the next callback (ui_wl_pre_present).
    s->awaiting_frame = true;

    ui_display_list *dl = ui_mailbox_back(&s->mailbox);
    ui_display_list_reset(dl);
    dl->surface = s->handle;
    dl->frame_id = ++s->next_frame_id;
    dl->predicted_present_host_ns = s->predicted_host_ns;
    dl->anim_time_ns = now;
    dl->scale_120 = s->config.scale_120;
    dl->logical_width = (float)s->config.logical_width;
    dl->logical_height = (float)s->config.logical_height;
    dl->width = physical(s->config.logical_width, s->config.scale_120);
    dl->height = physical(s->config.logical_height, s->config.scale_120);
    dl->platform_token = s->config.token;
    memcpy(dl->clear, clear_color, sizeof dl->clear);
    if (ui_scene_build(&c->scene, s->handle, dl) != UI_OK)
        UI_ERROR(c->log, MODULE, "display list incomplete (out of memory)");
    if (dl->rect_count)
        UI_TRACE_LOG(c->log, MODULE, "frame %llu t=%.3f s: first rect at %.1f,%.1f",
                     (unsigned long long)dl->frame_id, (double)now / 1e9, dl->rects[0].x,
                     dl->rects[0].y);
    if (ui_mailbox_publish(&s->mailbox)) c->stats.display_lists_replaced++;
    c->stats.frames_committed++;

    if (s->resized_pending) {
        s->resized_pending = false;
        ui_wire_event_surface_resized(&c->events, s->handle, (float)s->config.logical_width,
                                      (float)s->config.logical_height, s->config.scale_120);
    }
    ui_wire_event_frame_committed(&c->events, s->handle, dl->frame_id, now);
}

static void frame(ui_core *c) {
    // One animation time for the whole frame: the earliest predicted present among the
    // surfaces drawing now. Per-surface sampling arrives with multi-monitor support.
    uint64_t host = UINT64_MAX;
    ui_core_surface *first = NULL;
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        ui_core_surface *s = &c->surfaces[i];
        if (ui_handle_is_null(s->handle) || !s->frame_ready) continue;
        if (!first) first = s;
        if (s->predicted_host_ns < host) host = s->predicted_host_ns;
    }
    if (!first) return;
    uint64_t now = ui_clock_at(&c->clock, host);
    if (now < c->last_anim_ns) now = c->last_anim_ns; // never run time backwards
    c->last_anim_ns = now;

    // Nothing to apply and no ready surface with work: every ready surface is idle, and
    // skipping costs no apply, animate or GPU work.
    bool busy = c->pending_count > 0;
    for (uint32_t i = 0; i < UI_MAX_SURFACES && !busy; i++) {
        ui_core_surface *s = &c->surfaces[i];
        if (!ui_handle_is_null(s->handle) && s->frame_ready && surface_has_work(c, s)) busy = true;
    }
    ui_handle main = first->handle;
    uint64_t frame_id = first->next_frame_id + 1;
    if (busy) {
        for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
            ui_core_surface *s = &c->surfaces[i];
            if (!ui_handle_is_null(s->handle) && s->frame_ready)
                UI_HOOK(c->hooks, UI_TRACE_FRAME_BEGIN, 0, s->handle, s->next_frame_id + 1);
        }

        UI_HOOK_PHASE_BEGIN(c->hooks, UI_PHASE_APPLY, main, frame_id);
        for (uint32_t i = 0; i < c->pending_count; i++) {
            uint64_t before = c->scene.stats.applied;
            if (ui_scene_apply(&c->scene, c->pending[i].data, c->pending[i].size, now) != UI_OK)
                UI_ERROR(c->log, MODULE, "rejected a malformed command buffer");
            c->stats.commands_applied += c->scene.stats.applied - before;
            free(c->pending[i].data);
        }
        c->pending_count = 0;
        c->stats.commands_dropped = c->scene.stats.dropped;
        c->stats.buffers_rejected = c->scene.stats.rejected;
        UI_HOOK_PHASE_END(c->hooks, UI_PHASE_APPLY, main, frame_id);

        UI_HOOK_PHASE_BEGIN(c->hooks, UI_PHASE_ANIMATE, main, frame_id);
        ui_scene_animate(&c->scene, now);
        UI_HOOK_PHASE_END(c->hooks, UI_PHASE_ANIMATE, main, frame_id);
    }

    UI_HOOK_PHASE_BEGIN(c->hooks, UI_PHASE_COMMIT, main, frame_id);
    bool drew = false;
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        ui_core_surface *s = &c->surfaces[i];
        if (ui_handle_is_null(s->handle) || !s->frame_ready) continue;
        s->frame_ready = false;
        if (busy && surface_has_work(c, s)) {
            commit(c, s, now);
            drew = true;
        } else {
            // Idle: no commit, and no callback until work arrives (see wake_idle).
            s->awaiting_frame = false;
            c->stats.frames_skipped_idle++;
            if (busy) UI_HOOK(c->hooks, UI_TRACE_FRAME_END, 0, s->handle, s->next_frame_id + 1);
            UI_HOOK(c->hooks, UI_TRACE_FRAME_IDLE, 0, s->handle, s->next_frame_id);
        }
    }
    UI_HOOK_PHASE_END(c->hooks, UI_PHASE_COMMIT, main, frame_id);
    if (drew) ui_signal_notify(&c->render->signal);
}

// Idle surfaces with new work ask the platform for a frame callback. Pending command buffers
// count as work for every surface: which one they touch is only known once applied, at a
// frame boundary.
static void wake_idle(ui_core *c) {
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        ui_core_surface *s = &c->surfaces[i];
        if (ui_handle_is_null(s->handle) || !s->configured) continue;
        if (s->frame_ready || s->awaiting_frame) continue;
        if (c->pending_count == 0 && !surface_has_work(c, s)) continue;
        s->awaiting_frame = true;
        c->platform->vt->surface_request_frame(s->platform);
    }
}

static void flush_events(ui_core *c) {
    if (!c->events.op_count || c->events_in_flight >= MAX_EVENTS_IN_FLIGHT) return;
    size_t size;
    uint8_t *bytes = ui_wire_buf_finish(&c->events, &size);
    if (!bytes) return;
    ui_msg msg = {.type = UI_MSG_EVENT_BUFFER, .ptr = bytes, .size = size};
    // Cannot be full: at most MAX_EVENTS_IN_FLIGHT buffers are ever queued.
    if (ui_queue_push(&c->logic->queue, &msg) != UI_OK) {
        UI_ERROR(c->log, MODULE, "logic queue full; dropped events");
        free(bytes);
        return;
    }
    c->events_in_flight++;
}

void ui_core_step(void *self) {
    ui_core *c = self;
    drain(c);
    fire_timers(c);
    frame(c);
    wake_idle(c);
    flush_events(c);
}

uint64_t ui_core_wait_timeout_ns(ui_core *c) {
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        const ui_core_surface *s = &c->surfaces[i];
        if (!ui_handle_is_null(s->handle) && s->frame_ready) return 0;
    }
    if (ui_queue_count(&c->from_logic) || ui_queue_count(&c->actor.queue)) return 0;
    if (c->events.op_count && c->events_in_flight < MAX_EVENTS_IN_FLIGHT) return 0;
    uint64_t next = UINT64_MAX;
    for (uint32_t i = 0; i < c->timer_count; i++) {
        if (c->timers[i].deadline_ns < next) next = c->timers[i].deadline_ns;
    }
    if (next == UINT64_MAX) return UINT64_MAX;
    uint64_t at = ui_clock_host_at(&c->clock, next), now = ui_clock_host_now(&c->clock);
    if (at == UINT64_MAX) return UINT64_MAX; // paused: only stepping moves the clock
    return at > now ? at - now : 0;
}

bool ui_core_frames_done(const ui_core *c) {
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        const ui_core_surface *s = &c->surfaces[i];
        if (!ui_handle_is_null(s->handle) && s->last_done_frame < s->next_frame_id) return false;
    }
    return true;
}

bool ui_core_idle(const ui_core *c) {
    if (c->events_in_flight || c->events.op_count) return false;
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        const ui_core_surface *s = &c->surfaces[i];
        if (ui_handle_is_null(s->handle)) continue;
        if (s->last_done_frame < s->next_frame_id) return false;
    }
    return true;
}
