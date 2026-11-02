#include "runtime/logic_actor.h"

#include <stdlib.h>
#include <time.h>

#define MODULE "logic"

// The logic thread may wait for room in the core's queue; the core never waits on logic.
static ui_result push_to_core(ui_logic *l, const ui_msg *msg) {
    for (int attempt = 0; attempt < 1000; attempt++) {
        ui_result r = ui_queue_push(l->core_queue, msg);
        if (r != UI_ERR_FULL) return r;
        thrd_sleep(&(struct timespec){.tv_nsec = 1000000}, NULL);
    }
    return UI_ERR_FULL;
}

static void flush(ui_logic *l) {
    if (!ui_cmd_writer_op_count(l->writer)) return;
    uint8_t *bytes;
    size_t size;
    if (ui_cmd_writer_take(l->writer, &bytes, &size) != UI_OK) return;
    ui_msg msg = {.type = UI_MSG_CMD_BUFFER, .ptr = bytes, .size = size};
    if (push_to_core(l, &msg) != UI_OK) {
        UI_ERROR(l->log, MODULE, "core queue full; dropped a command buffer");
        free(bytes);
    }
}

static ui_result logic_start(void *self) {
    ui_logic *l = self;
    ui_result r = ui_cmd_writer_create(&l->writer);
    if (r != UI_OK) return r;
    if (l->desc.start) {
        r = l->desc.start(l->desc.user, l->writer);
        if (r != UI_OK) {
            // Like a JS exception at startup: reported, and the app keeps running.
            UI_ERROR(l->log, MODULE, "logic start failed: %s", ui_result_str(r));
        }
    }
    flush(l);
    return UI_OK;
}

static void logic_step(void *self) {
    ui_logic *l = self;
    ui_msg msg;
    while (ui_queue_pop(&l->actor.queue, &msg)) {
        if (msg.type != UI_MSG_EVENT_BUFFER) {
            ui_msg_drop(&msg);
            continue;
        }
        if (l->desc.on_events) {
            ui_result r = l->desc.on_events(l->desc.user, msg.ptr, msg.size, l->writer);
            if (r != UI_OK) UI_ERROR(l->log, MODULE, "logic failed: %s", ui_result_str(r));
        }
        free(msg.ptr);
        flush(l);
        // Sent after the commands, so when the core sees it they are already queued.
        ui_msg done = {.type = UI_MSG_EVENTS_DONE, .u64 = 1};
        if (push_to_core(l, &done) != UI_OK)
            UI_ERROR(l->log, MODULE, "core queue full; lost an events-done notice");
    }
}

static void logic_stop(void *self) {
    ui_logic *l = self;
    if (l->desc.stop) l->desc.stop(l->desc.user);
    ui_cmd_writer_destroy(l->writer);
    l->writer = NULL;
}

ui_result ui_logic_init(ui_logic *l, const ui_logic_desc *desc, const ui_logger *log,
                        ui_queue *core_queue) {
    l->desc = *desc;
    l->log = log;
    l->core_queue = core_queue;
    l->writer = NULL;
    return ui_actor_init(&l->actor, "logic", l, logic_start, logic_step, logic_stop, 256,
                         (ui_waker){0});
}

void ui_logic_destroy(ui_logic *l) { ui_actor_destroy(&l->actor); }
