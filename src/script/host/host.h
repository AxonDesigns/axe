// The script host: runs an app's bundle on the logic thread through the engine interface
// (src/script/engine.h), in turns driven by event buffers (docs/script.md, "The turn").
// Engine-independent: nothing here names QuickJS.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hooks/hooks_internal.h"
#include "runtime/common.h"
#include "script/engine.h"
#include "ui/script.h"

// The wire timer the host multiplexes JS timers onto (docs/script.md, "Timers").
#define UI_SCRIPT_TIMER_ID 0xFFFFFFFFu

// For Axe's own tools (ui_script_desc.internal).
struct ui_script_internal {
    // Called after the host's globals and `axe:host` exist, before the bundle: defines more
    // modules or globals (the test runner's `axe:test`).
    void (*setup)(void *user, ui_js *js);
    void *user;
    // performance.now() reads the animation clock instead of wall time (the test runner),
    // so tests are deterministic.
    bool performance_from_animation_clock;
};

typedef struct ui_script_env {
    const ui_logger *log;
    ui_hooks *hooks;
    uint64_t start_ns; // CLOCK_MONOTONIC when the app started: performance.now()'s origin
} ui_script_env;

typedef struct ui_script_host ui_script_host;

// Thread: the logic thread, which the host then belongs to. Reads the bundle; the app's code
// runs in the first turn. The desc is copied, except the bundle's bytes.
ui_result ui_script_host_create(const ui_script_desc *desc, const ui_script_env *env, ui_script_host **out);
void ui_script_host_destroy(ui_script_host *h);

// One turn (docs/script.md, "The turn"): `events` in, and in *cmds the turn's command buffer
// (malloc'd, the caller frees it), or NULL when there's nothing to send.
void ui_script_host_turn(ui_script_host *h, const uint8_t *events, size_t size, uint8_t **cmds, size_t *cmds_size);

// The engine, for tests that look inside (memory, the value stack).
ui_js *ui_script_host_engine(ui_script_host *h);

// ---- Bundles (docs/script.md, "Bundles") ----------------------------------------------------

#define UI_SCRIPT_BUNDLE_MAGIC 0x43425841u // "AXBC"
#define UI_SCRIPT_BUNDLE_VERSION 1u
#define UI_SCRIPT_BUNDLE_HEADER 16u

// The hash of the engine's name and version that bytecode records and loading checks.
uint32_t ui_script_engine_id(void);
// Wraps engine bytecode in the bundle header. Ownership: free() *out.
ui_result ui_script_bundle_wrap(const uint8_t *bytecode, size_t size, uint8_t **out, size_t *out_size);
// What a bundle holds: source (*is_bytecode false: the whole thing) or bytecode (after the
// header). UI_ERR_MALFORMED for a bad header; UI_ERR_UNSUPPORTED for bytecode from another
// engine or engine version, with *message saying which.
ui_result ui_script_bundle_open(const uint8_t *bytes, size_t size, const uint8_t **code, size_t *code_size,
                                bool *is_bytecode, const char **message);
