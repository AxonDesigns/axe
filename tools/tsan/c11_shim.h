// Force-included into every C file of a ThreadSanitizer build (UI_SANITIZE=thread,
// cmake/Sanitizers.cmake). glibc's C11 threads (thrd_create, mtx_*, cnd_*) call pthreads
// internally, out of TSan's sight, so every lock would look absent and every handoff a
// race. This maps them onto the pthread functions TSan intercepts. Only what the code uses
// is covered: plain mutexes, condition variables, and creating and joining threads.
#pragma once

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <threads.h>
#include <time.h>

static inline int ui_tsan_mtx_init(mtx_t *m, int type) {
    (void)type; // mtx_plain: nothing uses recursive or timed mutexes
    return pthread_mutex_init((pthread_mutex_t *)m, NULL) ? thrd_error : thrd_success;
}
static inline int ui_tsan_mtx_lock(mtx_t *m) {
    return pthread_mutex_lock((pthread_mutex_t *)m) ? thrd_error : thrd_success;
}
static inline int ui_tsan_mtx_unlock(mtx_t *m) {
    return pthread_mutex_unlock((pthread_mutex_t *)m) ? thrd_error : thrd_success;
}
static inline void ui_tsan_mtx_destroy(mtx_t *m) { pthread_mutex_destroy((pthread_mutex_t *)m); }
static inline int ui_tsan_cnd_init(cnd_t *c) {
    return pthread_cond_init((pthread_cond_t *)c, NULL) ? thrd_error : thrd_success;
}
static inline int ui_tsan_cnd_signal(cnd_t *c) {
    return pthread_cond_signal((pthread_cond_t *)c) ? thrd_error : thrd_success;
}
static inline int ui_tsan_cnd_broadcast(cnd_t *c) {
    return pthread_cond_broadcast((pthread_cond_t *)c) ? thrd_error : thrd_success;
}
static inline int ui_tsan_cnd_wait(cnd_t *c, mtx_t *m) {
    return pthread_cond_wait((pthread_cond_t *)c, (pthread_mutex_t *)m) ? thrd_error : thrd_success;
}
static inline int ui_tsan_cnd_timedwait(cnd_t *c, mtx_t *m, const struct timespec *t) {
    int r = pthread_cond_timedwait((pthread_cond_t *)c, (pthread_mutex_t *)m, t);
    return r == ETIMEDOUT ? thrd_timedout : r ? thrd_error : thrd_success;
}
static inline void ui_tsan_cnd_destroy(cnd_t *c) { pthread_cond_destroy((pthread_cond_t *)c); }

// thrd_start_t returns int, a pthread start routine void *: a trampoline carries the call.
struct ui_tsan_start {
    thrd_start_t fn;
    void *arg;
};
static inline void *ui_tsan_trampoline(void *p) {
    struct ui_tsan_start s = *(struct ui_tsan_start *)p;
    free(p);
    return (void *)(intptr_t)s.fn(s.arg);
}
static inline int ui_tsan_thrd_create(thrd_t *t, thrd_start_t fn, void *arg) {
    struct ui_tsan_start *s = malloc(sizeof *s);
    if (!s) return thrd_nomem;
    *s = (struct ui_tsan_start){fn, arg};
    if (pthread_create((pthread_t *)t, NULL, ui_tsan_trampoline, s)) {
        free(s);
        return thrd_error;
    }
    return thrd_success;
}
static inline int ui_tsan_thrd_join(thrd_t t, int *res) {
    void *r;
    if (pthread_join((pthread_t)t, &r)) return thrd_error;
    if (res) *res = (int)(intptr_t)r;
    return thrd_success;
}

#define mtx_init ui_tsan_mtx_init
#define mtx_lock ui_tsan_mtx_lock
#define mtx_unlock ui_tsan_mtx_unlock
#define mtx_destroy ui_tsan_mtx_destroy
#define cnd_init ui_tsan_cnd_init
#define cnd_signal ui_tsan_cnd_signal
#define cnd_broadcast ui_tsan_cnd_broadcast
#define cnd_wait ui_tsan_cnd_wait
#define cnd_timedwait ui_tsan_cnd_timedwait
#define cnd_destroy ui_tsan_cnd_destroy
#define thrd_create ui_tsan_thrd_create
#define thrd_join ui_tsan_thrd_join
