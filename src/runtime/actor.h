// Actors: a message queue plus start/step/stop functions. An actor never blocks in step();
// the scheduler decides whether it runs on its own thread or in sequence on the main
// thread, and the actor cannot tell the difference.
#pragma once

#include <stdatomic.h>
#include <threads.h>

#include "runtime/queue.h"

typedef struct ui_actor {
    const char *name;
    void *self;
    ui_result (*start)(void *self); // on the thread that will run step()
    void (*step)(void *self);       // drain the queue and do pending work, without blocking
    void (*stop)(void *self);       // on the same thread as start(), after the last step()
    ui_queue queue;
    ui_signal signal;               // notified on every push; a threaded actor waits on it

    // Scheduler-owned.
    thrd_t thread;
    bool has_thread;
    _Atomic bool stopping;
    _Atomic bool start_done;
    ui_result start_result;
} ui_actor;

// A queue_capacity of UI_QUEUE_UNBOUNDED makes the actor's queue grow instead of refusing
// messages (for inboxes whose messages must never be lost).
#define UI_QUEUE_UNBOUNDED 0u

// `waker` overrides the default (notify `signal`) for actors that sleep elsewhere, such as
// the core, which sleeps in the platform's event wait.
ui_result ui_actor_init(ui_actor *a, const char *name, void *self, ui_result (*start)(void *),
                        void (*step)(void *), void (*stop)(void *), uint32_t queue_capacity,
                        ui_waker waker);
void ui_actor_destroy(ui_actor *a);

// Frees the payload a message owns (buffers, descriptors).
void ui_msg_drop(ui_msg *msg);
