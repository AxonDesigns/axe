#include "runtime/actor.h"

#include <stdlib.h>

ui_result ui_actor_init(ui_actor *a, const char *name, void *self, ui_result (*start)(void *),
                        void (*step)(void *), void (*stop)(void *), uint32_t queue_capacity,
                        ui_waker waker) {
    a->name = name;
    a->self = self;
    a->start = start;
    a->step = step;
    a->stop = stop;
    a->has_thread = false;
    a->start_result = UI_OK;
    atomic_init(&a->stopping, false);
    atomic_init(&a->start_done, false);
    ui_result r = ui_signal_init(&a->signal);
    if (r != UI_OK) return r;
    if (!waker.wake) waker = (ui_waker){ui_signal_wake_fn, &a->signal};
    r = queue_capacity == UI_QUEUE_UNBOUNDED ? ui_queue_init_unbounded(&a->queue, 64, waker)
                                             : ui_queue_init(&a->queue, queue_capacity, waker);
    if (r != UI_OK) ui_signal_destroy(&a->signal);
    return r;
}

void ui_actor_destroy(ui_actor *a) {
    ui_queue_destroy(&a->queue, ui_msg_drop);
    ui_signal_destroy(&a->signal);
}

void ui_msg_drop(ui_msg *msg) {
    switch (msg->type) {
    case UI_MSG_CMD_BUFFER:
    case UI_MSG_EVENT_BUFFER:
    case UI_MSG_SURFACE_ADD:
    case UI_MSG_ATLAS_UPDATE:
        free(msg->ptr);
        break;
    default:
        break; // capture requests are owned by the requester
    }
    msg->ptr = NULL;
}
