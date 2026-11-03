// The logic actor: hosts the language runtime, either C logic (ui_logic_desc) or the script
// host (ui_script_desc, src/script/host/). It receives event buffers, runs the logic, and
// flushes each resulting command buffer to the core. It never touches core state directly.
#pragma once

#include "hooks/hooks_internal.h"
#include "runtime/actor.h"
#include "runtime/common.h"
#include "ui/app.h"

typedef struct ui_script_host ui_script_host;

typedef struct ui_logic {
    ui_actor actor;
    ui_logic_desc desc;
    const ui_logger *log;
    ui_queue *core_queue;    // the core's unbounded logic channel
    ui_cmd_writer *writer;
    ui_msg *outbox;          // messages not yet accepted by the core (only on OOM)
    uint32_t outbox_count, outbox_capacity;
    bool reported_oom;
    // A script app: its desc (copied), the hooks it reports to, and the host, created on the
    // logic thread when the actor starts.
    ui_script_desc *script;
    ui_hooks *hooks;
    uint64_t start_ns;
    ui_script_host *host;
} ui_logic;

// `script` (optional) replaces `desc`'s callbacks.
ui_result ui_logic_init(ui_logic *l, const ui_logic_desc *desc, const ui_script_desc *script,
                        const ui_logger *log, ui_hooks *hooks, ui_queue *core_queue);
void ui_logic_destroy(ui_logic *l);
