// The clock event (docs/command-buffer.md, "Events"): first in every event buffer, sent once
// at startup even with nothing else to say, stamped with the animation clock when the buffer
// leaves, never going backwards, and following a paused clock as it is stepped. Both
// schedulers.
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "ui/ui.h"

typedef struct record {
    int buffers, clocks_first, clocks_elsewhere;
    uint64_t last_clock, max_frame_time;
    bool backwards;
    uint64_t clocks[64];
} record;

// A timer every 10 ms keeps buffers coming while nothing draws (an idle surface sends none).
static ui_result on_events(void *user, const void *events, size_t size, ui_cmd_writer *cmds) {
    record *r = user;
    if (!r->buffers) ui_cmd_set_timer(cmds, 1, 10000000);
    ui_event_reader rd;
    ui_result res = ui_event_reader_init(&rd, events, size);
    ui_event ev;
    int index = 0;
    while (res == UI_OK && (res = ui_event_next(&rd, &ev)) == UI_OK) {
        if (ev.type == UI_EVENT_CLOCK) {
            if (index == 0) r->clocks_first++;
            else r->clocks_elsewhere++;
            if (ev.clock.now_ns < r->last_clock) r->backwards = true;
            r->last_clock = ev.clock.now_ns;
            if (r->buffers < 64) r->clocks[r->buffers] = ev.clock.now_ns;
        } else if (ev.type == UI_EVENT_TIMER_FIRED) {
            ui_cmd_set_timer(cmds, 1, ev.timer_fired.deadline_ns + 10000000);
        } else if (ev.type == UI_EVENT_FRAME_COMMITTED) {
            // The clock is never behind a time the buffer reports.
            if (ev.frame_committed.predicted_present_ns > r->last_clock) r->backwards = true;
        }
        index++;
    }
    r->buffers++;
    return res == UI_ERR_END ? UI_OK : res;
}

static void run(ui_scheduler_kind scheduler) {
    record r = {0};
    ui_app_desc desc = {
        .scheduler = scheduler,
        .surface = {.kind = UI_SURFACE_HEADLESS, .width = 64, .height = 64},
        .logic = {.user = &r, .on_events = on_events},
        .start_paused = true,
    };
    ui_app *app;
    CHECK(ui_app_create(&desc, &app) == UI_OK);
    CHECK(ui_app_frame(app) == UI_OK); // startup: surface_resized, behind a clock
    CHECK(r.buffers >= 1 && r.clocks_first == r.buffers && r.clocks[0] == 0);
    // A stepped clock: each buffer after the step carries the new time.
    for (int i = 1; i <= 5; i++) {
        ui_app_clock_step(app, 10000000);
        CHECK(ui_app_frame(app) == UI_OK);
    }
    CHECK(r.clocks_first == r.buffers && r.clocks_elsewhere == 0);
    CHECK(!r.backwards);
    // Every clock is a time the stepped clock had when its buffer left.
    CHECK(r.buffers >= 3);
    for (int i = 0; i < r.buffers && i < 64; i++) CHECK(r.clocks[i] % 10000000u == 0);
    CHECK(r.last_clock > 0 && r.last_clock <= ui_app_clock_now(app));
    ui_app_destroy(app);
}

static void test_threaded(void) { run(UI_SCHEDULER_THREADED); }
static void test_single(void) { run(UI_SCHEDULER_SINGLE); }

int main(void) {
    RUN(test_threaded);
    RUN(test_single);
    return test_failures ? 1 : 0;
}
