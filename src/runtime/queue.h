// Actor messaging: a bounded multi-producer queue, a wake signal and the display-list mailbox.
#pragma once

#include <stdatomic.h>
#include <stdint.h>
#include <threads.h>

#include "ui/handle.h"
#include "ui/result.h"

typedef enum ui_msg_type {
    // logic -> core
    UI_MSG_CMD_BUFFER = 1,  // ptr: malloc'd command buffer (ownership moves), size: bytes
    UI_MSG_EVENTS_DONE,     // u64: event buffers fully handled (and their commands flushed)
    // core -> logic
    UI_MSG_EVENT_BUFFER,    // ptr: malloc'd event buffer (ownership moves), size: bytes
    // core -> render
    UI_MSG_SURFACE_ADD,     // ptr: ui_render_surface_desc* (ownership moves)
    UI_MSG_SURFACE_REMOVE,  // surface
    UI_MSG_CAPTURE,         // ptr: ui_capture_request* (owned by the requester)
    // render -> core
    UI_MSG_FRAME_DONE,      // surface, u64: frame id
    UI_MSG_RENDER_FAILED,   // u64: ui_result
} ui_msg_type;

typedef struct ui_msg {
    ui_msg_type type;
    ui_handle surface;
    uint64_t u64;
    void *ptr;
    size_t size;
} ui_msg;

// Wakes whoever waits for a queue; called after every successful push, outside the lock.
typedef struct ui_waker {
    void (*wake)(void *user);
    void *user;
} ui_waker;

typedef struct ui_queue {
    mtx_t lock;
    ui_msg *ring;
    uint32_t capacity, head, count;
    bool unbounded; // grows instead of returning UI_ERR_FULL
    ui_waker waker;
} ui_queue;

ui_result ui_queue_init(ui_queue *q, uint32_t capacity, ui_waker waker);
// A queue that never refuses a message: it doubles its ring when full. For channels whose
// messages must not be lost (logic -> core); fails only when memory runs out.
ui_result ui_queue_init_unbounded(ui_queue *q, uint32_t initial_capacity, ui_waker waker);
// Frees owned payloads of undelivered messages via `drop` (may be NULL).
void ui_queue_destroy(ui_queue *q, void (*drop)(ui_msg *msg));
// Thread: any. Never blocks. A bounded queue returns UI_ERR_FULL; an unbounded one grows
// and returns UI_ERR_OUT_OF_MEMORY only if that fails.
ui_result ui_queue_push(ui_queue *q, const ui_msg *msg);
// Thread: any. Messages currently queued.
uint32_t ui_queue_count(ui_queue *q);
// Thread: the owning actor. Returns false when empty.
bool ui_queue_pop(ui_queue *q, ui_msg *out);

// Edge-free wake signal: waiters remember the sequence they saw before doing work, so a
// notify between their check and their wait is never lost.
typedef struct ui_signal {
    mtx_t lock;
    cnd_t cond;
    uint64_t seq;
} ui_signal;

ui_result ui_signal_init(ui_signal *s);
void ui_signal_destroy(ui_signal *s);
void ui_signal_notify(ui_signal *s);       // thread: any
uint64_t ui_signal_seq(ui_signal *s);      // thread: any
// Blocks until the sequence differs from `seen` or `timeout_ns` elapses (0: forever).
void ui_signal_wait(ui_signal *s, uint64_t seen, uint64_t timeout_ns);
void ui_signal_wake_fn(void *signal);      // ui_waker adapter

// Triple-buffered mailbox for immutable display lists, one per surface. The producer (core)
// writes into its back slot and publishes it; the consumer (render) takes the newest
// published slot. Publishing over an untaken slot replaces it: lists are never queued.
typedef struct ui_mailbox {
    _Atomic uint32_t middle; // slot index | UI_MAILBOX_FRESH
    uint32_t back;           // producer-owned
    uint32_t front;          // consumer-owned
    void *slots[3];
} ui_mailbox;

#define UI_MAILBOX_FRESH 4u

void ui_mailbox_init(ui_mailbox *m, void *slot0, void *slot1, void *slot2);
// Producer: the slot to fill next.
void *ui_mailbox_back(ui_mailbox *m);
// Producer: publishes the back slot. Returns true if it replaced an untaken one.
bool ui_mailbox_publish(ui_mailbox *m);
// Consumer: the newest published slot, or NULL if nothing new since the last take.
void *ui_mailbox_take(ui_mailbox *m);
