// The logic actor: hosts the language runtime (for now a C stand-in described by
// ui_logic_desc). It receives event buffers, runs the logic, and flushes each resulting
// command buffer to the core. It never touches core state directly.
#pragma once

#include "runtime/actor.h"
#include "runtime/common.h"
#include "ui/app.h"

typedef struct ui_logic {
    ui_actor actor;
    ui_logic_desc desc;
    const ui_logger *log;
    ui_queue *core_queue;
    ui_cmd_writer *writer;
} ui_logic;

ui_result ui_logic_init(ui_logic *l, const ui_logic_desc *desc, const ui_logger *log,
                        ui_queue *core_queue);
void ui_logic_destroy(ui_logic *l);
