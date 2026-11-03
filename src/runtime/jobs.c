#include "runtime/jobs.h"

#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <unistd.h>

#define MODULE "jobs"
#define MAX_THREADS 64u

typedef struct job {
    ui_job_fn fn;
    void *data;
    ui_job_group *group;
} job;

struct ui_jobs {
    const ui_logger *log;
    uint32_t threads;
    thrd_t workers[MAX_THREADS];
    mtx_t lock;
    cnd_t wake;
    job *ring;                // FIFO: earlier submissions run first
    uint32_t capacity, head, count;
    bool stopping;
};

uint32_t ui_jobs_default_threads(uint32_t requested) {
    const char *env = getenv("UI_WORKERS");
    if (env && *env) {
        char *end;
        unsigned long v = strtoul(env, &end, 10);
        if (!*end) requested = v > MAX_THREADS ? MAX_THREADS : (uint32_t)v;
        if (!*end && v == 0) return 0;
    }
    if (requested) return requested > MAX_THREADS ? MAX_THREADS : requested;
    long hw = sysconf(_SC_NPROCESSORS_ONLN);
    long n = hw - 3;
    return n < 1 ? 1 : n > 4 ? 4 : (uint32_t)n;
}

void ui_job_group_init(ui_job_group *g, void (*done)(void *user), void *user) {
    atomic_init(&g->pending, 1u);
    g->done = done;
    g->user = user;
}

static void finish(ui_job_group *g) {
    // Read the callback first: once the count reaches 0 the owner may free the group.
    void (*done)(void *) = g->done;
    void *user = g->user;
    if (atomic_fetch_sub_explicit(&g->pending, 1u, memory_order_acq_rel) == 1 && done)
        done(user);
}

void ui_job_group_seal(ui_job_group *g) { finish(g); }

void ui_job_group_reopen(ui_job_group *g) {
    atomic_fetch_add_explicit(&g->pending, 1u, memory_order_acq_rel);
}

bool ui_job_group_idle(const ui_job_group *g) {
    return atomic_load_explicit(&g->pending, memory_order_acquire) == 0;
}

static void run(ui_jobs *j, job jb, uint32_t worker, bool cancelled) {
    (void)j;
    ui_job_ctx ctx = {worker, cancelled};
    jb.fn(jb.data, &ctx);
    finish(jb.group);
}

static int worker_main(void *arg) {
    ui_jobs *j = ((void **)arg)[0];
    uint32_t index = (uint32_t)(uintptr_t)((void **)arg)[1];
    free(arg);
    mtx_lock(&j->lock);
    for (;;) {
        while (!j->count && !j->stopping) cnd_wait(&j->wake, &j->lock);
        if (!j->count) break; // stopping, and nothing left to cancel
        job jb = j->ring[j->head];
        j->head = (j->head + 1) % j->capacity;
        j->count--;
        bool cancelled = j->stopping;
        mtx_unlock(&j->lock);
        run(j, jb, index, cancelled);
        mtx_lock(&j->lock);
    }
    mtx_unlock(&j->lock);
    return 0;
}

ui_result ui_jobs_create(const ui_jobs_desc *desc, ui_jobs **out) {
    ui_jobs *j = calloc(1, sizeof *j);
    if (!j) return UI_ERR_OUT_OF_MEMORY;
    j->log = desc->log;
    if (mtx_init(&j->lock, mtx_plain) != thrd_success) {
        free(j);
        return UI_ERR_OUT_OF_MEMORY;
    }
    if (cnd_init(&j->wake) != thrd_success) {
        mtx_destroy(&j->lock);
        free(j);
        return UI_ERR_OUT_OF_MEMORY;
    }
    uint32_t want = desc->threads > MAX_THREADS ? MAX_THREADS : desc->threads;
    for (uint32_t i = 0; i < want; i++) {
        void **arg = malloc(2 * sizeof *arg);
        if (!arg) break;
        arg[0] = j;
        arg[1] = (void *)(uintptr_t)i;
        if (thrd_create(&j->workers[i], worker_main, arg) != thrd_success) {
            free(arg);
            break;
        }
        j->threads++;
    }
    if (j->threads < want) {
        UI_ERROR(j->log, MODULE, "started %u of %u worker threads", j->threads, want);
        ui_jobs_destroy(j);
        return UI_ERR_OUT_OF_MEMORY;
    }
    *out = j;
    return UI_OK;
}

void ui_jobs_destroy(ui_jobs *j) {
    if (!j) return;
    mtx_lock(&j->lock);
    j->stopping = true;
    cnd_broadcast(&j->wake);
    mtx_unlock(&j->lock);
    for (uint32_t i = 0; i < j->threads; i++) thrd_join(j->workers[i], NULL);
    // Inline pools never queue; a pool whose threads failed to start may have queued jobs.
    while (j->count) {
        job jb = j->ring[j->head];
        j->head = (j->head + 1) % j->capacity;
        j->count--;
        run(j, jb, j->threads, true);
    }
    free(j->ring);
    cnd_destroy(&j->wake);
    mtx_destroy(&j->lock);
    free(j);
}

uint32_t ui_jobs_threads(const ui_jobs *j) { return j->threads; }

static bool grow_ring(ui_jobs *j) {
    uint32_t cap = j->capacity ? j->capacity * 2 : 64;
    job *ring = malloc(cap * sizeof *ring);
    if (!ring) return false;
    for (uint32_t i = 0; i < j->count; i++) ring[i] = j->ring[(j->head + i) % j->capacity];
    free(j->ring);
    j->ring = ring;
    j->capacity = cap;
    j->head = 0;
    return true;
}

ui_result ui_jobs_submit(ui_jobs *j, ui_job_group *g, ui_job_fn fn, void *data) {
    atomic_fetch_add_explicit(&g->pending, 1u, memory_order_relaxed);
    job jb = {fn, data, g};
    if (!j->threads) {
        run(j, jb, 0, false);
        return UI_OK;
    }
    mtx_lock(&j->lock);
    if (j->count == j->capacity && !grow_ring(j)) {
        mtx_unlock(&j->lock);
        atomic_fetch_sub_explicit(&g->pending, 1u, memory_order_relaxed);
        return UI_ERR_OUT_OF_MEMORY;
    }
    j->ring[(j->head + j->count) % j->capacity] = jb;
    j->count++;
    cnd_signal(&j->wake);
    mtx_unlock(&j->lock);
    return UI_OK;
}
