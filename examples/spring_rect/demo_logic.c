#include "demo_logic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>

static const float anchor_centers[DEMO_ANCHORS][2] = {
    {0.25f, 0.3f}, {0.75f, 0.3f}, {0.75f, 0.7f}, {0.25f, 0.7f}};

static const float anchor_colors[DEMO_ANCHORS][4] = {
    {0.95f, 0.45f, 0.25f, 1.0f}, // orange
    {0.25f, 0.70f, 0.95f, 1.0f}, // sky
    {0.55f, 0.85f, 0.35f, 1.0f}, // green
    {0.85f, 0.35f, 0.80f, 1.0f}, // magenta
};

void demo_logic_rect(uint32_t anchor, float width, float height, float out[4]) {
    const float *c = anchor_centers[anchor % DEMO_ANCHORS];
    float w = width * 0.3f, h = height * 0.3f;
    out[0] = c[0] * width - w * 0.5f;
    out[1] = c[1] * height - h * 0.5f;
    out[2] = w;
    out[3] = h;
}

void demo_logic_color(uint32_t anchor, float out[4]) {
    memcpy(out, anchor_colors[anchor % DEMO_ANCHORS], sizeof anchor_colors[0]);
}

float demo_logic_radius(float width, float height) {
    return (width < height ? width : height) * 0.08f;
}

ui_result demo_logic_init(demo_logic *d) {
    memset(d, 0, sizeof *d);
    const char *stall = getenv("UI_STALL_LOGIC");
    d->stall = stall && stall[0] && strcmp(stall, "0") != 0;
    d->period_ns = DEMO_PERIOD_NS;
    return ui_handle_allocator_create(&d->handles);
}

void demo_logic_fini(demo_logic *d) { ui_handle_allocator_destroy(d->handles); }

static ui_result on_resized(demo_logic *d, const ui_event *ev, ui_cmd_writer *cmds) {
    d->surface = ev->surface;
    d->width = ev->surface_resized.width;
    d->height = ev->surface_resized.height;
    float rect[4], color[4];
    demo_logic_rect(d->anchor, d->width, d->height, rect);
    ui_result r = UI_OK;
    if (ui_handle_is_null(d->node)) {
        r = ui_handle_alloc(d->handles, &d->node);
        if (r == UI_OK) r = ui_cmd_create_node(cmds, d->node, d->surface);
        demo_logic_color(d->anchor, color);
        if (r == UI_OK) r = ui_cmd_set_color(cmds, d->node, color[0], color[1], color[2], color[3]);
        if (r == UI_OK) r = ui_cmd_set_rect(cmds, d->node, rect[0], rect[1], rect[2], rect[3]);
    } else {
        // Follow the window: spring to the anchor's place in the new size.
        r = ui_cmd_start_spring(cmds, d->node, UI_PROP_RECT, rect, DEMO_STIFFNESS, DEMO_DAMPING);
    }
    if (r == UI_OK)
        r = ui_cmd_set_corner_radius(cmds, d->node, demo_logic_radius(d->width, d->height));
    return r;
}

static ui_result on_frame(demo_logic *d, const ui_event *ev, ui_cmd_writer *cmds) {
    if (d->stall_pending) {
        // The spring sent last turn is now running in the core. Block this actor like a
        // slow logic turn would; the core must keep animating without us.
        d->stall_pending = false;
        fprintf(stderr, "[demo] stalling logic for 500 ms\n");
        thrd_sleep(&(struct timespec){.tv_nsec = 500000000}, NULL);
    }
    if (d->timer_started) return UI_OK;
    // The first frame gives us the clock; from here a core timer drives the cycle, so the
    // demo keeps going while the window is idle and commits no frames.
    d->timer_started = true;
    d->next_switch_ns = ev->frame_committed.predicted_present_ns + d->period_ns;
    return ui_cmd_set_timer(cmds, DEMO_TIMER_ID, d->next_switch_ns);
}

static ui_result on_timer(demo_logic *d, const ui_event *ev, ui_cmd_writer *cmds) {
    if (ev->timer_fired.id != DEMO_TIMER_ID || ui_handle_is_null(d->node)) return UI_OK;
    // Re-arm from the deadline, not from when it fired, so the period does not drift; skip
    // ahead if the clock jumped past several periods.
    d->next_switch_ns = ev->timer_fired.deadline_ns + d->period_ns;
    if (d->next_switch_ns <= ev->timer_fired.now_ns)
        d->next_switch_ns = ev->timer_fired.now_ns + d->period_ns;
    d->anchor = (d->anchor + 1) % DEMO_ANCHORS;
    float rect[4], color[4];
    demo_logic_rect(d->anchor, d->width, d->height, rect);
    demo_logic_color(d->anchor, color);
    ui_result r = ui_cmd_start_spring(cmds, d->node, UI_PROP_RECT, rect, DEMO_STIFFNESS, DEMO_DAMPING);
    if (r == UI_OK)
        r = ui_cmd_start_spring(cmds, d->node, UI_PROP_COLOR, color, DEMO_STIFFNESS, DEMO_DAMPING);
    if (r == UI_OK) r = ui_cmd_set_timer(cmds, DEMO_TIMER_ID, d->next_switch_ns);
    if (r == UI_OK && d->stall) d->stall_pending = true;
    return r;
}

static ui_result on_events(void *user, const void *events, size_t size, ui_cmd_writer *cmds) {
    demo_logic *d = user;
    ui_event_reader reader;
    ui_result r = ui_event_reader_init(&reader, events, size);
    if (r != UI_OK) return r;
    ui_event ev;
    while ((r = ui_event_next(&reader, &ev)) == UI_OK) {
        switch (ev.type) {
        case UI_EVENT_SURFACE_RESIZED: r = on_resized(d, &ev, cmds); break;
        case UI_EVENT_FRAME_COMMITTED: r = on_frame(d, &ev, cmds); break;
        case UI_EVENT_TIMER_FIRED: r = on_timer(d, &ev, cmds); break;
        case UI_EVENT_SURFACE_CLOSE_REQUESTED:
        case UI_EVENT_FONT_LOADED:
        case UI_EVENT_LAYOUT_MEASURED: break;
        }
        if (r != UI_OK) return r;
    }
    return r == UI_ERR_END ? UI_OK : r;
}

ui_logic_desc demo_logic_desc(demo_logic *d) {
    return (ui_logic_desc){.user = d, .on_events = on_events};
}
