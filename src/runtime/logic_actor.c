#include "runtime/logic_actor.h"

#include <stdlib.h>
#include <string.h>

#include "script/host/host.h"

#define MODULE "logic"

// Sends everything waiting in the outbox, in order. The channel to the core is unbounded,
// so a push fails only when memory runs out; the message then stays in the outbox and is
// retried on the next step instead of being dropped.
static void send_outbox(ui_logic *l) {
    uint32_t sent = 0;
    while (sent < l->outbox_count && ui_queue_push(l->core_queue, &l->outbox[sent]) == UI_OK) sent++;
    if (sent < l->outbox_count && !l->reported_oom) {
        UI_ERROR(l->log, MODULE, "out of memory sending to the core; %u message(s) held",
                 l->outbox_count - sent);
        l->reported_oom = true;
    }
    if (sent == l->outbox_count) l->reported_oom = false;
    memmove(l->outbox, l->outbox + sent, (l->outbox_count - sent) * sizeof *l->outbox);
    l->outbox_count -= sent;
}

static void send(ui_logic *l, ui_msg msg) {
    // Queue behind anything already held so the core sees messages in order.
    if (ui_grow((void **)&l->outbox, &l->outbox_capacity, l->outbox_count + 1,
                sizeof *l->outbox) != UI_OK) {
        // Only reachable when the process is out of memory twice over.
        UI_ERROR(l->log, MODULE, "out of memory; lost a message to the core");
        ui_msg_drop(&msg);
        return;
    }
    l->outbox[l->outbox_count++] = msg;
    send_outbox(l);
}

static void flush(ui_logic *l) {
    if (!l->writer || !ui_cmd_writer_op_count(l->writer)) return;
    uint8_t *bytes;
    size_t size;
    if (ui_cmd_writer_take(l->writer, &bytes, &size) != UI_OK) return;
    send(l, (ui_msg){.type = UI_MSG_CMD_BUFFER, .ptr = bytes, .size = size, .u64 = ui_monotonic_ns()});
}

static ui_result logic_start(void *self) {
    ui_logic *l = self;
    if (l->script) {
        // Like a JS exception at startup: reported, and the app keeps running (empty).
        ui_script_env env = {.log = l->log, .hooks = l->hooks, .start_ns = l->start_ns};
        ui_result r = l->runtime->create(l->script, &env, l->runtime->config, &l->host);
        if (r != UI_OK) UI_ERROR(l->log, MODULE, "the script host failed to start: %s", ui_result_str(r));
        return UI_OK;
    }
    ui_result r = ui_cmd_writer_create(&l->writer);
    if (r != UI_OK) return r;
    if (l->desc.start) {
        r = l->desc.start(l->desc.user, l->writer);
        if (r != UI_OK) {
            // Like a JS exception at startup: reported, and the app keeps running.
            UI_ERROR(l->log, MODULE, "logic start failed: %s", ui_result_str(r));
        }
    }
    flush(l);
    return UI_OK;
}

static void logic_step(void *self) {
    ui_logic *l = self;
    if (l->outbox_count) send_outbox(l);
    ui_msg msg;
    while (ui_queue_pop(&l->actor.queue, &msg)) {
        if (msg.type != UI_MSG_EVENT_BUFFER) {
            ui_msg_drop(&msg);
            continue;
        }
        if (l->script) {
            uint8_t *cmds = NULL;
            size_t size = 0;
            if (l->host) l->runtime->turn(l->host, msg.ptr, msg.size, &cmds, &size);
            // u64: when it was sent, for the hook API's COMMANDS_APPLIED.
            if (cmds) send(l, (ui_msg){.type = UI_MSG_CMD_BUFFER, .ptr = cmds, .size = size, .u64 = ui_monotonic_ns()});
        } else if (l->desc.on_events) {
            ui_result r = l->desc.on_events(l->desc.user, msg.ptr, msg.size, l->writer);
            if (r != UI_OK) UI_ERROR(l->log, MODULE, "logic failed: %s", ui_result_str(r));
        }
        free(msg.ptr);
        flush(l);
        // Sent after the commands, so when the core sees it they are already queued.
        send(l, (ui_msg){.type = UI_MSG_EVENTS_DONE, .u64 = 1});
    }
}

static void logic_stop(void *self) {
    ui_logic *l = self;
    if (l->host) l->runtime->destroy(l->host);
    l->host = NULL;
    if (l->desc.stop) l->desc.stop(l->desc.user);
    ui_cmd_writer_destroy(l->writer);
    l->writer = NULL;
}

static void free_script(ui_script_desc *d) {
    if (!d) return;
    free((void *)(uintptr_t)d->bundle_path);
    free((void *)(uintptr_t)d->bundle_name);
    for (uint32_t i = 0; i < d->arg_count; i++) free((void *)(uintptr_t)d->args[i]);
    free((void *)(uintptr_t)d->args);
    free(d);
}

static char *copy_str(const char *s, bool *ok) {
    if (!s) return NULL;
    char *c = strdup(s);
    if (!c) *ok = false;
    return c;
}

// The desc's strings and arguments are copied; the bundle's bytes are not.
static ui_script_desc *copy_script(const ui_script_desc *src) {
    ui_script_desc *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    *d = *src;
    bool ok = true;
    d->bundle_path = copy_str(src->bundle_path, &ok);
    d->bundle_name = copy_str(src->bundle_name, &ok);
    d->args = NULL;
    d->arg_count = 0;
    if (ok && src->arg_count) {
        char **args = calloc(src->arg_count, sizeof *args);
        d->args = (const char *const *)args;
        ok = args != NULL;
        for (uint32_t i = 0; ok && i < src->arg_count; i++, d->arg_count++)
            args[i] = copy_str(src->args[i] ? src->args[i] : "", &ok);
    }
    if (!ok) {
        free_script(d);
        return NULL;
    }
    return d;
}

ui_result ui_logic_init(ui_logic *l, const ui_logic_desc *desc, const ui_script_desc *script,
                        const ui_logger *log, ui_hooks *hooks, ui_queue *core_queue) {
    if (script && (desc->start || desc->on_events || desc->stop)) return UI_ERR_INVALID_ARGUMENT;
    l->script = NULL;
    l->host = NULL;
    l->runtime = script && script->internal && script->internal->runtime ? script->internal->runtime : &ui_script_runtime_js;
    l->hooks = hooks;
    l->start_ns = ui_monotonic_ns();
    if (script && !(l->script = copy_script(script))) return UI_ERR_OUT_OF_MEMORY;
    l->desc = *desc;
    l->log = log;
    l->core_queue = core_queue;
    l->writer = NULL;
    l->outbox = NULL;
    l->outbox_count = l->outbox_capacity = 0;
    l->reported_oom = false;
    return ui_actor_init(&l->actor, "logic", l, logic_start, logic_step, logic_stop, 256,
                         (ui_waker){0});
}

void ui_logic_destroy(ui_logic *l) {
    free_script(l->script);
    l->script = NULL;
    for (uint32_t i = 0; i < l->outbox_count; i++) ui_msg_drop(&l->outbox[i]);
    free(l->outbox);
    ui_actor_destroy(&l->actor);
}
