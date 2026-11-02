#include "runtime/queue.h"

#include <stdlib.h>
#include <time.h>

ui_result ui_queue_init(ui_queue *q, uint32_t capacity, ui_waker waker) {
    q->ring = calloc(capacity, sizeof *q->ring);
    if (!q->ring) return UI_ERR_OUT_OF_MEMORY;
    if (mtx_init(&q->lock, mtx_plain) != thrd_success) {
        free(q->ring);
        return UI_ERR_OUT_OF_MEMORY;
    }
    q->capacity = capacity;
    q->head = 0;
    q->count = 0;
    q->unbounded = false;
    q->waker = waker;
    return UI_OK;
}

ui_result ui_queue_init_unbounded(ui_queue *q, uint32_t initial_capacity, ui_waker waker) {
    ui_result r = ui_queue_init(q, initial_capacity, waker);
    if (r == UI_OK) q->unbounded = true;
    return r;
}

// Doubles the ring, unwrapping it so the oldest message moves to index 0. Lock held.
static ui_result grow(ui_queue *q) {
    if (q->capacity > UINT32_MAX / 2) return UI_ERR_OUT_OF_MEMORY;
    uint32_t cap = q->capacity * 2;
    ui_msg *ring = malloc((size_t)cap * sizeof *ring);
    if (!ring) return UI_ERR_OUT_OF_MEMORY;
    for (uint32_t i = 0; i < q->count; i++) ring[i] = q->ring[(q->head + i) % q->capacity];
    free(q->ring);
    q->ring = ring;
    q->capacity = cap;
    q->head = 0;
    return UI_OK;
}

void ui_queue_destroy(ui_queue *q, void (*drop)(ui_msg *msg)) {
    ui_msg msg;
    while (ui_queue_pop(q, &msg)) {
        if (drop) drop(&msg);
    }
    mtx_destroy(&q->lock);
    free(q->ring);
}

ui_result ui_queue_push(ui_queue *q, const ui_msg *msg) {
    mtx_lock(&q->lock);
    if (q->count == q->capacity) {
        ui_result r = q->unbounded ? grow(q) : UI_ERR_FULL;
        if (r != UI_OK) {
            mtx_unlock(&q->lock);
            return r;
        }
    }
    q->ring[(q->head + q->count) % q->capacity] = *msg;
    q->count++;
    mtx_unlock(&q->lock);
    if (q->waker.wake) q->waker.wake(q->waker.user);
    return UI_OK;
}

uint32_t ui_queue_count(ui_queue *q) {
    mtx_lock(&q->lock);
    uint32_t n = q->count;
    mtx_unlock(&q->lock);
    return n;
}

bool ui_queue_pop(ui_queue *q, ui_msg *out) {
    mtx_lock(&q->lock);
    bool any = q->count > 0;
    if (any) {
        *out = q->ring[q->head];
        q->head = (q->head + 1) % q->capacity;
        q->count--;
    }
    mtx_unlock(&q->lock);
    return any;
}

ui_result ui_signal_init(ui_signal *s) {
    if (mtx_init(&s->lock, mtx_plain) != thrd_success) return UI_ERR_OUT_OF_MEMORY;
    if (cnd_init(&s->cond) != thrd_success) {
        mtx_destroy(&s->lock);
        return UI_ERR_OUT_OF_MEMORY;
    }
    s->seq = 0;
    return UI_OK;
}

void ui_signal_destroy(ui_signal *s) {
    cnd_destroy(&s->cond);
    mtx_destroy(&s->lock);
}

void ui_signal_notify(ui_signal *s) {
    mtx_lock(&s->lock);
    s->seq++;
    mtx_unlock(&s->lock);
    cnd_broadcast(&s->cond);
}

uint64_t ui_signal_seq(ui_signal *s) {
    mtx_lock(&s->lock);
    uint64_t seq = s->seq;
    mtx_unlock(&s->lock);
    return seq;
}

void ui_signal_wait(ui_signal *s, uint64_t seen, uint64_t timeout_ns) {
    struct timespec deadline;
    if (timeout_ns) {
        timespec_get(&deadline, TIME_UTC);
        uint64_t ns = (uint64_t)deadline.tv_nsec + timeout_ns;
        deadline.tv_sec += (time_t)(ns / 1000000000u);
        deadline.tv_nsec = (long)(ns % 1000000000u);
    }
    mtx_lock(&s->lock);
    while (s->seq == seen) {
        if (timeout_ns) {
            if (cnd_timedwait(&s->cond, &s->lock, &deadline) == thrd_timedout) break;
        } else {
            cnd_wait(&s->cond, &s->lock);
        }
    }
    mtx_unlock(&s->lock);
}

void ui_signal_wake_fn(void *signal) { ui_signal_notify(signal); }

void ui_mailbox_init(ui_mailbox *m, void *slot0, void *slot1, void *slot2) {
    m->slots[0] = slot0;
    m->slots[1] = slot1;
    m->slots[2] = slot2;
    m->back = 0;
    atomic_init(&m->middle, 1u);
    m->front = 2;
}

void *ui_mailbox_back(ui_mailbox *m) { return m->slots[m->back]; }

bool ui_mailbox_publish(ui_mailbox *m) {
    // acq_rel: release our writes to the back slot, acquire the consumer's release of the
    // slot we get back so we never write into a list it is still reading.
    uint32_t old = atomic_exchange_explicit(&m->middle, m->back | UI_MAILBOX_FRESH,
                                            memory_order_acq_rel);
    m->back = old & 3u;
    return (old & UI_MAILBOX_FRESH) != 0;
}

void *ui_mailbox_take(ui_mailbox *m) {
    if (!(atomic_load_explicit(&m->middle, memory_order_relaxed) & UI_MAILBOX_FRESH)) return NULL;
    uint32_t old = atomic_exchange_explicit(&m->middle, m->front, memory_order_acq_rel);
    m->front = old & 3u;
    return m->slots[m->front];
}
