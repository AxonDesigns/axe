// The runtime host interface (docs/script.md, "Runtime hosts"): what the logic actor needs from a
// language's runtime, and what every runtime shares. Nothing here names a language or an engine.
// JavaScript's host is host.c; a language with another runtime (bindings/luau) has its own.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hooks/hooks_internal.h"
#include "runtime/common.h"
#include "ui/script.h"
#include "wire/wire.h"

// The wire timer every runtime multiplexes its language's timers onto (docs/script.md, "Timers").
#define UI_SCRIPT_TIMER_ID 0xFFFFFFFFu

typedef struct ui_script_env {
    const ui_logger *log;
    ui_hooks *hooks;
    uint64_t start_ns; // CLOCK_MONOTONIC when the app started
} ui_script_env;

// A language's runtime. All three run on the logic thread, in this order: create, one turn per
// event buffer, destroy.
typedef struct ui_script_runtime {
    // Reads the bundle; the app's code runs in the first turn. The desc is copied, except the
    // bundle's bytes. `config` is this struct's.
    ui_result (*create)(const ui_script_desc *desc, const ui_script_env *env, void *config, void **out);
    // One turn (docs/script.md, "The turn"): `events` in, and in *cmds the turn's command buffer
    // (malloc'd, the caller frees it), or NULL when there's nothing to send.
    void (*turn)(void *host, const uint8_t *events, size_t size, uint8_t **cmds, size_t *cmds_size);
    void (*destroy)(void *host);
    void *config;
} ui_script_runtime;

// The JavaScript runtime (host.c): what ui_script_desc runs unless it says otherwise.
extern const ui_script_runtime ui_script_runtime_js;

// ---- Timers (docs/script.md, "Timers") ------------------------------------------------------

#define UI_SCRIPT_NO_DEADLINE UINT64_MAX

// `fn` and `args` belong to the runtime: what it needs to call the timer back. An interval's
// stay valid between firings; ui_script_timers_clear and a popped timeout hand them back.
typedef struct ui_script_timer {
    uint64_t deadline, seq, interval; // interval 0: a timeout
    uint32_t id;
    void *fn, *args;
} ui_script_timer;

typedef struct ui_script_timers {
    ui_script_timer *heap; // a binary heap by (deadline, seq)
    uint32_t count, capacity;
    uint64_t next_seq;
    uint32_t next_id;
    uint64_t armed;        // the wire timer's deadline, UI_SCRIPT_NO_DEADLINE when disarmed
} ui_script_timers;

void ui_script_timers_init(ui_script_timers *t);
void ui_script_timers_free(ui_script_timers *t); // the storage: the runtime releases its `fn`s
// A timer `ms` of the animation clock after `turn_ns`: NaN and negative delays are 0, and an
// interval is at least 1 ms. False when out of memory.
bool ui_script_timers_add(ui_script_timers *t, uint64_t turn_ns, double ms, bool repeat, void *fn, void *args,
                          uint32_t *id);
// Removes timer `id`; false if there is none.
bool ui_script_timers_clear(ui_script_timers *t, double id, ui_script_timer *out);
// The next timer to run this turn, earliest first and ties in creation order; false when none is
// due. `batch` is next_seq when the turn's timers started: timers set by their callbacks wait for
// a later turn, even with a 0 delay. An interval is already re-armed, unless out of memory (then
// its `interval` reads 0 and it ends here, as a timeout does).
bool ui_script_timers_pop(ui_script_timers *t, uint64_t turn_ns, uint64_t batch, const ui_logger *log,
                          ui_script_timer *out);
// True for the wire timer's own event, which then counts as disarmed: run the due timers instead
// of passing it to the language.
bool ui_script_timers_fired(ui_script_timers *t, const ui_wire_op *op);
// Ends a turn: appends the wire timer's re-arm to its command buffer (*out, made if the language
// sent none) if the next deadline changed.
void ui_script_timers_arm(ui_script_timers *t, const ui_logger *log, uint8_t **out, size_t *out_size);

// ---- The turn --------------------------------------------------------------------------------

// The turn's time: `reader`'s first event, which is the clock (docs/command-buffer.md). Taken by
// value, so the reader stays at the start. *turn_ns only moves forward.
void ui_script_turn_time(ui_wire_reader reader, uint64_t *turn_ns);

// ---- Reporting ---------------------------------------------------------------------------------

// To stderr and the hooks: "<prefix><name>: <message> (<context>)" and the stack. `name` and
// `stack` may be "".
void ui_script_report(const ui_script_env *env, ui_trace_kind kind, const char *prefix, const char *name,
                      const char *message, const char *stack, const char *context);
// Reports a task that started at `start_ns` if it took longer than `threshold_ns`.
void ui_script_report_long_task(const ui_script_env *env, const char *what, uint64_t start_ns, uint64_t threshold_ns);

// ---- Files -------------------------------------------------------------------------------------

// The whole file (malloc'd), with one spare byte; NULL if it can't be read.
uint8_t *ui_script_read_file(const char *path, size_t *size);
