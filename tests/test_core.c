// Core pacing tests against a fake platform: idle surfaces stop drawing and ask the platform
// for exactly one frame callback when work arrives, and a frame the renderer had to skip is
// retried after the platform's refresh period.
#include <stdlib.h>
#include <string.h>

#include "core/core.h"
#include "test.h"

#define FAKE_PERIOD_NS 7000000u // a 143 Hz display, deliberately not 60 Hz

typedef struct fake_platform {
    ui_platform base;
    int frame_requests;
    uint64_t token;
} fake_platform;

struct ui_psurface {
    void *core_tag;
};

static fake_platform fake;
static uint64_t fake_now;

static uint64_t fake_time(void *user) {
    (void)user;
    return fake_now;
}

static void fake_destroy(ui_platform *p) { (void)p; }

static void fake_configure(ui_psurface *s, uint32_t width) {
    ui_surface_config config = {.logical_width = width, .logical_height = 100, .scale_120 = 120,
                                .token = ++fake.token};
    fake.base.listener.configure(fake.base.listener.user, s->core_tag, &config);
}

static ui_result fake_surface_create(ui_platform *p, const ui_surface_desc *desc, void *tag,
                                     ui_psurface **out) {
    (void)p;
    ui_psurface *s = malloc(sizeof *s);
    if (!s) return UI_ERR_OUT_OF_MEMORY;
    s->core_tag = tag;
    *out = s;
    fake_configure(s, desc->width);
    return UI_OK;
}

static void fake_surface_destroy(ui_platform *p, ui_psurface *s) {
    (void)p;
    free(s);
}

static void fake_native(ui_psurface *s, ui_native_surface *out) {
    (void)s;
    *out = (ui_native_surface){.kind = UI_NATIVE_OFFSCREEN};
}

static void fake_pre_present(ui_psurface *s, const ui_present_info *info) { (void)s, (void)info; }

static ui_result fake_set_position(ui_psurface *s, int32_t x, int32_t y) {
    (void)s, (void)x, (void)y;
    return UI_ERR_UNSUPPORTED;
}

static void fake_request_frame(ui_psurface *s) {
    (void)s;
    fake.frame_requests++;
}

static uint64_t fake_period(ui_psurface *s) {
    (void)s;
    return FAKE_PERIOD_NS;
}

static ui_result fake_wait(ui_platform *p, uint64_t timeout_ns) {
    (void)p, (void)timeout_ns;
    return UI_OK;
}

static void fake_wake(ui_platform *p) { (void)p; }

static const ui_platform_vtable fake_vtable = {
    .destroy = fake_destroy,
    .surface_create = fake_surface_create,
    .surface_destroy = fake_surface_destroy,
    .surface_native = fake_native,
    .surface_pre_present = fake_pre_present,
    .surface_set_position = fake_set_position,
    .surface_request_frame = fake_request_frame,
    .surface_period_ns = fake_period,
    .wait = fake_wait,
    .wake = fake_wake,
};

typedef struct harness {
    ui_logger log;
    ui_hooks hooks;
    ui_signal progress;
    ui_actor logic, render; // stand-ins: only their queues are used
    ui_core core;
    ui_handle surface;
    ui_core_surface *s;
    ui_handle_allocator *handles;
    ui_cmd_writer *cmds;
} harness;

static void setup(harness *h) {
    memset(h, 0, sizeof *h);
    memset(&fake, 0, sizeof fake);
    fake_now = 1000000000u;
    h->log.level = UI_LOG_ERROR;
    ui_hooks_init(&h->hooks);
    ui_signal_init(&h->progress);
    ui_actor_init(&h->logic, "logic", NULL, NULL, NULL, NULL, 16, (ui_waker){0});
    ui_actor_init(&h->render, "render", NULL, NULL, NULL, NULL, 16, (ui_waker){0});
    fake.base.vt = &fake_vtable;
    fake.base.log = &h->log;
    ui_core_desc desc = {.log = &h->log, .hooks = &h->hooks, .platform = &fake.base,
                         .time = {fake_time, NULL}, .progress = &h->progress};
    CHECK(ui_core_init(&h->core, &desc) == UI_OK);
    fake.base.listener = ui_core_platform_listener(&h->core);
    h->core.logic = &h->logic;
    h->core.render = &h->render;
    ui_surface_desc sd = {.kind = UI_SURFACE_HEADLESS, .width = 100, .height = 100};
    CHECK(ui_core_add_surface(&h->core, &sd, &h->surface) == UI_OK);
    h->s = ui_core_surface_get(&h->core, h->surface);
    ui_handle_allocator_create(&h->handles);
    ui_cmd_writer_create(&h->cmds);
}

static void teardown(harness *h) {
    ui_cmd_writer_destroy(h->cmds);
    ui_handle_allocator_destroy(h->handles);
    ui_core_remove_surfaces(&h->core);
    ui_core_destroy(&h->core);
    ui_actor_destroy(&h->render);
    ui_actor_destroy(&h->logic);
    ui_signal_destroy(&h->progress);
}

// The compositor's frame callback.
static void callback(harness *h) {
    fake.base.listener.frame(fake.base.listener.user, h->s, fake_now + FAKE_PERIOD_NS);
}

static void send_commands(harness *h) {
    uint8_t *bytes;
    size_t size;
    CHECK(ui_cmd_writer_take(h->cmds, &bytes, &size) == UI_OK);
    ui_msg msg = {.type = UI_MSG_CMD_BUFFER, .ptr = bytes, .size = size};
    CHECK(ui_queue_push(&h->core.from_logic, &msg) == UI_OK);
}

static const ui_display_list *take_list(harness *h) { return ui_mailbox_take(&h->s->mailbox); }

static void test_idle_surface_requests_one_callback(void) {
    harness h;
    setup(&h);
    // First frame: the new surface must draw (its size is news to logic).
    callback(&h);
    ui_core_step(&h.core);
    CHECK_EQ_INT(h.core.stats.frames_committed, 1);
    CHECK(take_list(&h) != NULL);
    CHECK(h.s->awaiting_frame); // the drawn frame's present asks for the next callback

    // Next callback with nothing changed: no commit, and the surface goes idle.
    callback(&h);
    ui_core_step(&h.core);
    CHECK_EQ_INT(h.core.stats.frames_committed, 1);
    CHECK_EQ_INT(h.core.stats.frames_skipped_idle, 1);
    CHECK(!h.s->awaiting_frame && !h.s->frame_ready);
    CHECK(take_list(&h) == NULL);
    for (int i = 0; i < 3; i++) ui_core_step(&h.core); // idle stays idle: no requests
    CHECK_EQ_INT(fake.frame_requests, 0);
    CHECK_EQ_INT(ui_core_wait_timeout_ns(&h.core), UINT64_MAX);

    // Work arrives: exactly one frame callback is requested, however often the core steps.
    ui_handle node;
    ui_handle_alloc(h.handles, &node);
    ui_cmd_create_node(h.cmds, node, h.surface);
    ui_cmd_set_rect(h.cmds, node, 10, 10, 20, 20);
    ui_cmd_set_color(h.cmds, node, 1, 1, 1, 1);
    send_commands(&h);
    CHECK_EQ_INT(ui_core_wait_timeout_ns(&h.core), 0);
    ui_core_step(&h.core);
    ui_core_step(&h.core);
    CHECK_EQ_INT(fake.frame_requests, 1);
    CHECK_EQ_INT(h.core.stats.frames_committed, 1); // applied only at the frame boundary

    // The callback comes: the commands apply and the frame is drawn.
    callback(&h);
    ui_core_step(&h.core);
    CHECK_EQ_INT(h.core.stats.frames_committed, 2);
    CHECK_EQ_INT(h.core.stats.commands_applied, 3);
    const ui_display_list *dl = take_list(&h);
    CHECK(dl && dl->rect_count == 1);

    // A configure while idle (window state, same size) also wakes it once to be acked.
    callback(&h);
    ui_core_step(&h.core); // idle again
    fake_configure(h.s->platform, 100);
    ui_core_step(&h.core);
    CHECK_EQ_INT(fake.frame_requests, 2);
    callback(&h);
    ui_core_step(&h.core);
    CHECK_EQ_INT(h.core.stats.frames_committed, 3);
    teardown(&h);
}

static void test_spring_keeps_surface_drawing(void) {
    harness h;
    setup(&h);
    ui_handle node;
    ui_handle_alloc(h.handles, &node);
    ui_cmd_create_node(h.cmds, node, h.surface);
    ui_cmd_set_rect(h.cmds, node, 0, 0, 10, 10);
    ui_cmd_set_color(h.cmds, node, 1, 1, 1, 1);
    float target[4] = {50, 0, 10, 10};
    ui_cmd_start_spring(h.cmds, node, UI_PROP_RECT, target, 170, 18.25f);
    send_commands(&h);
    uint64_t committed = 0;
    for (int i = 0; i < 300; i++) { // 300 frames at 7 ms = 2.1 s
        fake_now += FAKE_PERIOD_NS;
        callback(&h);
        ui_core_step(&h.core);
        take_list(&h);
        if (h.core.stats.frames_committed == committed) break; // first idle frame
        committed = h.core.stats.frames_committed;
    }
    // Every frame drew while the spring ran (~1.2 s at these constants), then it went idle.
    CHECK(committed > 100 && committed < 250);
    CHECK_EQ_INT(h.core.stats.frames_skipped_idle, 1);
    teardown(&h);
}

static void test_skipped_frame_retries_at_platform_period(void) {
    harness h;
    setup(&h);
    callback(&h);
    ui_core_step(&h.core);
    const ui_display_list *dl = take_list(&h);
    CHECK(dl != NULL);
    uint64_t frame = dl ? dl->frame_id : 0;

    // The renderer could not get a swapchain image: no frame callback will come for it.
    fake_now = 5000000000u;
    ui_msg done = {.type = UI_MSG_FRAME_DONE, .surface = h.surface, .u64 = frame, .size = 1};
    CHECK(ui_queue_push(&h.core.actor.queue, &done) == UI_OK);
    ui_core_step(&h.core);
    // It redraws on its own (nothing changed, but the content was lost), predicted one
    // period of *this* surface out, not a hard-coded 60 Hz.
    dl = take_list(&h);
    CHECK(dl != NULL);
    if (dl) CHECK_EQ_INT(dl->predicted_present_host_ns, 5000000000u + FAKE_PERIOD_NS);
    CHECK_EQ_INT(h.core.stats.frames_committed, 2);
    teardown(&h);
}

static void test_timer_wait_and_fire(void) {
    harness h;
    setup(&h);
    callback(&h);
    ui_core_step(&h.core);
    ui_cmd_set_timer(h.cmds, 7, 1500000000u); // animation time; the clock started at 0
    send_commands(&h);
    callback(&h);
    ui_core_step(&h.core); // applies set_timer at the frame boundary
    // The core sleeps exactly until the deadline (the fake clock is running, scale 1).
    uint64_t wait = ui_core_wait_timeout_ns(&h.core);
    CHECK_EQ_INT(wait, 1500000000u);
    // Drop the event buffers sent so far, then advance past the deadline.
    ui_msg m;
    while (ui_queue_pop(&h.logic.queue, &m)) ui_msg_drop(&m);
    h.core.events_in_flight = 0;
    fake_now += wait;
    ui_core_step(&h.core);
    CHECK(ui_queue_pop(&h.logic.queue, &m));
    ui_event_reader r;
    ui_event ev;
    bool fired = false;
    if (ui_event_reader_init(&r, m.ptr, m.size) == UI_OK) {
        while (ui_event_next(&r, &ev) == UI_OK) {
            if (ev.type == UI_EVENT_TIMER_FIRED && ev.timer_fired.id == 7 &&
                ev.timer_fired.deadline_ns == 1500000000u)
                fired = true;
        }
    }
    ui_msg_drop(&m);
    CHECK(fired);
    CHECK_EQ_INT(h.core.timer_count, 0); // one-shot
    teardown(&h);
}

int main(void) {
    RUN(test_idle_surface_requests_one_callback);
    RUN(test_spring_keeps_surface_drawing);
    RUN(test_skipped_frame_retries_at_platform_period);
    RUN(test_timer_wait_and_fire);
    return test_failures ? 1 : 0;
}
