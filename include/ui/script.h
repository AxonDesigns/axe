// Apps in a script language: a bundle run on the logic thread by the script host
// (docs/script.md). The host talks to the core only through the command and event buffers,
// as C logic (ui_logic_desc) does. JavaScript (QuickJS-ng) is the only language so far.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Axe's own tools (the TypeScript test runner) extend the host through this; apps leave it
// NULL. Declared in src/script/host/host.h.
typedef struct ui_script_internal ui_script_internal;

typedef struct ui_script_desc {
    // The bundle: an ES module (`.js`, from esbuild) or bytecode (`.axbc`, from axe-jsc),
    // told apart by its first bytes. Either a file, read at startup...
    const char *bundle_path;
    // ...or bytes in memory (an embedded bundle), which must outlive the app.
    const void *bundle;
    size_t bundle_size;
    const char *bundle_name;     // its name in stack traces; NULL: the path, or "app.js"
    const char *const *args;     // what @axe/core's app.args returns; copied
    uint32_t arg_count;
    size_t memory_limit;         // bytes the JS heap may use; 0: no limit
    uint64_t long_task_ns;       // report tasks longer than this; 0: 50 ms
    const ui_script_internal *internal;
} ui_script_desc;

// Bytecode is trusted input. The engine doesn't validate it, so loading bytecode that wasn't
// produced by this build's axe-jsc from a bundle you trust can corrupt memory. Never load
// bytecode from the network or any other source an attacker could control.

#ifdef __cplusplus
}
#endif
