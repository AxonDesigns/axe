// The app's worker pool: a few threads that run short, independent jobs for any module
// (text preparation first). Workers never touch the scene, the atlas or the GPU; a job works
// on data it was handed and publishes results through its own synchronization.
//
// Jobs belong to groups. A group counts its unfinished jobs and calls its `done` callback,
// on whichever thread finished the last one, when the count reaches zero after the group
// was sealed. Jobs never wait for other jobs; a job may submit follow-up jobs to its own
// group, which keeps the group open until they finish too.
//
// With zero threads (the single scheduler) a submitted job runs immediately on the
// submitting thread, so everything stays on one thread and deterministic.
#pragma once

#include <stdatomic.h>
#include <stdint.h>

#include "runtime/common.h"

typedef struct ui_jobs ui_jobs;

typedef struct ui_job_ctx {
    uint32_t worker;  // 0..threads-1 on a worker; `threads` when running inline
    bool cancelled;   // the pool is shutting down: release the job's data and return
} ui_job_ctx;

typedef void (*ui_job_fn)(void *data, const ui_job_ctx *ctx);

typedef struct ui_job_group {
    _Atomic uint32_t pending; // unfinished jobs, plus one until the group is sealed
    void (*done)(void *user);
    void *user;
} ui_job_group;

typedef struct ui_jobs_desc {
    const ui_logger *log;
    uint32_t threads; // 0: run every job inline on the submitting thread
} ui_jobs_desc;

// The thread count for "auto": hardware threads minus the core, logic and render threads,
// between 1 and 4. UI_WORKERS overrides `requested` (0 = auto).
uint32_t ui_jobs_default_threads(uint32_t requested);

ui_result ui_jobs_create(const ui_jobs_desc *desc, ui_jobs **out);
// Runs queued jobs with `cancelled` set, waits for running ones, joins the workers.
void ui_jobs_destroy(ui_jobs *jobs);
uint32_t ui_jobs_threads(const ui_jobs *jobs);

// Opens a group. `done` (may be NULL) runs once every job has finished after the group is
// sealed; it must not touch the group, which its owner may free as soon as it sees the
// group idle.
void ui_job_group_init(ui_job_group *g, void (*done)(void *user), void *user);
// Closes the group to new top-level submissions; follow-ups from its own jobs remain fine.
void ui_job_group_seal(ui_job_group *g);
// Opens a sealed group again (idle or not) for more top-level submissions; seal it again
// afterwards. Its `done` runs again once those finish. Thread: the group's owner.
void ui_job_group_reopen(ui_job_group *g);
// True once the group is sealed and every job has finished. Thread: any.
bool ui_job_group_idle(const ui_job_group *g);

// Queues `fn(data)` in group `g` (inline pools run it now). Thread: any, including a job of
// the same group. Fails only when memory runs out; the job then never runs.
ui_result ui_jobs_submit(ui_jobs *jobs, ui_job_group *g, ui_job_fn fn, void *data);
