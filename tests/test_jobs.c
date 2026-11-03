// The worker pool: groups, follow-up jobs, reopening, inline pools, shutdown with queued
// jobs.
#include <stdatomic.h>
#include <stdlib.h>
#include <threads.h>

#include "runtime/jobs.h"
#include "runtime/queue.h"
#include "test.h"

static ui_logger quiet = {UI_LOG_ERROR};

typedef struct counter {
    ui_jobs *jobs;
    ui_job_group *group;
    _Atomic uint32_t runs, cancelled, follow_ups;
} counter;

static void count_job(void *data, const ui_job_ctx *ctx) {
    counter *c = data;
    atomic_fetch_add(ctx->cancelled ? &c->cancelled : &c->runs, 1u);
}

// Submits two follow-ups into its own group; they keep the group open.
static void spawning_job(void *data, const ui_job_ctx *ctx) {
    counter *c = data;
    if (ctx->cancelled) return;
    atomic_fetch_add(&c->runs, 1u);
    for (int i = 0; i < 2; i++) {
        if (ui_jobs_submit(c->jobs, c->group, count_job, c) == UI_OK)
            atomic_fetch_add(&c->follow_ups, 1u);
    }
}

typedef struct waiter {
    ui_signal signal;
    _Atomic uint32_t done_calls;
} waiter;

static void on_done(void *user) {
    waiter *w = user;
    atomic_fetch_add(&w->done_calls, 1u);
    ui_signal_notify(&w->signal);
}

static void wait_idle(waiter *w, ui_job_group *g) {
    for (;;) {
        uint64_t seen = ui_signal_seq(&w->signal);
        if (ui_job_group_idle(g)) return;
        ui_signal_wait(&w->signal, seen, 0);
    }
}

static void test_inline(void) {
    ui_jobs *jobs;
    CHECK(ui_jobs_create(&(ui_jobs_desc){.log = &quiet, .threads = 0}, &jobs) == UI_OK);
    CHECK_EQ_INT(ui_jobs_threads(jobs), 0);
    waiter w = {0};
    ui_signal_init(&w.signal);
    ui_job_group g;
    counter c = {.jobs = jobs, .group = &g};
    ui_job_group_init(&g, on_done, &w);
    CHECK(ui_jobs_submit(jobs, &g, spawning_job, &c) == UI_OK);
    CHECK_EQ_INT(atomic_load(&c.runs), 3); // ran at once, with its follow-ups
    CHECK(!ui_job_group_idle(&g));          // not sealed yet
    CHECK_EQ_INT(atomic_load(&w.done_calls), 0);
    ui_job_group_seal(&g);
    CHECK(ui_job_group_idle(&g));
    CHECK_EQ_INT(atomic_load(&w.done_calls), 1);
    ui_jobs_destroy(jobs);
    ui_signal_destroy(&w.signal);
}

static void test_threads(void) {
    ui_jobs *jobs;
    CHECK(ui_jobs_create(&(ui_jobs_desc){.log = &quiet, .threads = 4}, &jobs) == UI_OK);
    CHECK_EQ_INT(ui_jobs_threads(jobs), 4);
    waiter w = {0};
    ui_signal_init(&w.signal);
    ui_job_group g;
    counter c = {.jobs = jobs, .group = &g};
    ui_job_group_init(&g, on_done, &w);
    for (int i = 0; i < 500; i++) CHECK(ui_jobs_submit(jobs, &g, spawning_job, &c) == UI_OK);
    ui_job_group_seal(&g);
    wait_idle(&w, &g);
    CHECK_EQ_INT(atomic_load(&c.runs), 500 + atomic_load(&c.follow_ups));
    CHECK_EQ_INT(atomic_load(&c.follow_ups), 1000);
    CHECK_EQ_INT(atomic_load(&w.done_calls), 1);

    // Reopened after going idle: more jobs, and `done` runs again.
    ui_job_group_reopen(&g);
    for (int i = 0; i < 10; i++) CHECK(ui_jobs_submit(jobs, &g, count_job, &c) == UI_OK);
    ui_job_group_seal(&g);
    wait_idle(&w, &g);
    CHECK_EQ_INT(atomic_load(&c.runs), 1510);
    CHECK_EQ_INT(atomic_load(&w.done_calls), 2);
    ui_jobs_destroy(jobs);
    ui_signal_destroy(&w.signal);
}

typedef struct gate {
    mtx_t lock;
    cnd_t cond;
    bool entered, open;
} gate;

static void blocking_job(void *data, const ui_job_ctx *ctx) {
    gate *g = data;
    if (ctx->cancelled) return;
    mtx_lock(&g->lock);
    g->entered = true;
    cnd_broadcast(&g->cond);
    while (!g->open) cnd_wait(&g->cond, &g->lock);
    mtx_unlock(&g->lock);
}

// Every queued job runs exactly once at shutdown, cancelled or not, so its data is freed.
static void test_shutdown(void) {
    ui_jobs *jobs;
    CHECK(ui_jobs_create(&(ui_jobs_desc){.log = &quiet, .threads = 1}, &jobs) == UI_OK);
    gate gt = {0};
    mtx_init(&gt.lock, mtx_plain);
    cnd_init(&gt.cond);
    ui_job_group g;
    counter c = {0};
    ui_job_group_init(&g, NULL, NULL);
    CHECK(ui_jobs_submit(jobs, &g, blocking_job, &gt) == UI_OK);
    for (int i = 0; i < 20; i++) CHECK(ui_jobs_submit(jobs, &g, count_job, &c) == UI_OK);
    ui_job_group_seal(&g);
    mtx_lock(&gt.lock);
    while (!gt.entered) cnd_wait(&gt.cond, &gt.lock);
    gt.open = true; // the queued 20 now run, or are cancelled if shutdown gets there first
    cnd_broadcast(&gt.cond);
    mtx_unlock(&gt.lock);
    ui_jobs_destroy(jobs);
    CHECK_EQ_INT(atomic_load(&c.runs) + atomic_load(&c.cancelled), 20);
    CHECK(ui_job_group_idle(&g));
    cnd_destroy(&gt.cond);
    mtx_destroy(&gt.lock);
}

static void test_default_threads(void) {
    uint32_t n = ui_jobs_default_threads(0);
    CHECK(n >= 1 && n <= 4);
    CHECK_EQ_INT(ui_jobs_default_threads(3), 3);
}

int main(void) {
    unsetenv("UI_WORKERS");
    RUN(test_inline);
    RUN(test_threads);
    RUN(test_shutdown);
    RUN(test_default_threads);
    return test_failures != 0;
}
