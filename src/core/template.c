#include "core/template.h"

#include <stdlib.h>
#include <string.h>

#include "wire/gen/wire_gen.h"
#include "wire/wire.h"

#define MODULE "template"

typedef struct ui_template {
    uint32_t id;          // 0: an empty map entry; TOMBSTONE: a destroyed one
    uint32_t node_count;
    uint32_t ops_size;
    uint8_t *ops;         // the framed ops, as defined
    bool *root;           // per node: created with a null parent (an instance root)
} ui_template;

#define TOMBSTONE UINT32_MAX

struct ui_templates {
    const ui_logger *log;
    ui_template *map; // open addressing by id
    uint32_t capacity, count, used; // used: entries not empty (live or tombstones)
    ui_template_stats stats;
};

ui_result ui_templates_create(const ui_logger *log, ui_templates **out) {
    ui_templates *t = calloc(1, sizeof *t);
    if (!t) return UI_ERR_OUT_OF_MEMORY;
    t->log = log;
    *out = t;
    return UI_OK;
}

static void free_entry(ui_template *e) {
    free(e->ops);
    free(e->root);
    e->ops = NULL;
    e->root = NULL;
}

void ui_templates_destroy(ui_templates *t) {
    if (!t) return;
    for (uint32_t i = 0; i < t->capacity; i++)
        if (t->map[i].id && t->map[i].id != TOMBSTONE) free_entry(&t->map[i]);
    free(t->map);
    free(t);
}

ui_template_stats ui_templates_stats(const ui_templates *t) { return t->stats; }

static uint32_t hash(uint32_t id) {
    id ^= id >> 16;
    id *= 0x7feb352du;
    id ^= id >> 15;
    return id;
}

static ui_template *find(ui_templates *t, uint32_t id) {
    if (!t->capacity) return NULL;
    for (uint32_t i = hash(id) & (t->capacity - 1);; i = (i + 1) & (t->capacity - 1)) {
        ui_template *e = &t->map[i];
        if (e->id == id) return e;
        if (!e->id) return NULL;
    }
}

static bool grow(ui_templates *t) {
    uint32_t cap = t->capacity ? t->capacity * 2 : 16;
    while (t->count * 2 >= cap) cap *= 2;
    ui_template *map = calloc(cap, sizeof *map);
    if (!map) return false;
    for (uint32_t i = 0; i < t->capacity; i++) {
        ui_template *e = &t->map[i];
        if (!e->id || e->id == TOMBSTONE) continue;
        uint32_t j = hash(e->id) & (cap - 1);
        while (map[j].id) j = (j + 1) & (cap - 1);
        map[j] = *e;
    }
    free(t->map);
    t->map = map;
    t->capacity = cap;
    t->used = t->count;
    return true;
}

static ui_template *insert(ui_templates *t, uint32_t id) {
    ui_template *e = find(t, id);
    if (e) return e;
    if ((t->used + 1) * 4 > t->capacity * 3 && !grow(t)) return NULL;
    uint32_t i = hash(id) & (t->capacity - 1);
    while (t->map[i].id && t->map[i].id != TOMBSTONE) i = (i + 1) & (t->capacity - 1);
    if (!t->map[i].id) t->used++;
    t->map[i] = (ui_template){.id = id};
    t->count++;
    return &t->map[i];
}

// ---- Definitions ------------------------------------------------------------------------------

static bool local_handle(const uint8_t *p, uint32_t node_count, uint32_t *index) {
    uint32_t idx = ui_wire_get_u32(p), gen = ui_wire_get_u32(p + 4);
    if (gen != UI_TEMPLATE_GENERATION || idx >= node_count) return false;
    *index = idx;
    return true;
}

// Checks a template's ops: framed, of the kinds a template holds, each node created once (with a
// null parent or one created before it) and named only after its creation. On success fills
// `root`.
static const char *check_ops(const uint8_t *ops, uint32_t size, uint32_t node_count, bool *root) {
    bool *created = calloc(node_count, sizeof *created);
    if (!created) return "out of memory";
    const char *why = NULL;
    uint32_t made = 0;
    for (uint32_t at = 0; at < size && !why;) {
        if (size - at < UI_WIRE_OP_HEADER_SIZE) {
            why = "an op runs past the ops";
            break;
        }
        uint16_t opcode = ui_wire_get_u16(ops + at), op_size = ui_wire_get_u16(ops + at + 2);
        if (op_size < UI_WIRE_OP_HEADER_SIZE || op_size % 4 || op_size > size - at) {
            why = "an op's size is wrong";
            break;
        }
        const uint8_t *payload = ops + at + UI_WIRE_OP_HEADER_SIZE;
        uint32_t payload_size = op_size - UI_WIRE_OP_HEADER_SIZE;
        switch (opcode) {
        case UI_OP_CREATE_NODE:
        case UI_OP_SET_STYLES:
        case UI_OP_SET_PROPS:
        case UI_OP_SET_TOKEN_OVERRIDES:
        case UI_OP_SET_TEXT:
        case UI_OP_SET_STATE: break;
        default: why = "an op a template can't hold"; continue;
        }
        if (!ui_wire_command_framed(opcode, payload, payload_size)) {
            why = "an op isn't framed";
            break;
        }
        uint32_t node;
        if (!local_handle(payload, node_count, &node)) {
            why = "an op names a node that isn't the template's";
            break;
        }
        if (opcode == UI_OP_CREATE_NODE) {
            uint32_t parent;
            if (created[node]) why = "a node is created twice";
            else if (ui_wire_get_u32(payload + 12) == 0 && ui_wire_get_u32(payload + 8) == 0) root[node] = true;
            else if (!local_handle(payload + 8, node_count, &parent) || !created[parent])
                why = "a node's parent isn't a node created before it, or null";
            created[node] = true;
            made++;
        } else if (!created[node]) {
            why = "an op names a node before it is created";
        }
        at += op_size;
    }
    if (!why && made != node_count) why = "not every node is created";
    free(created);
    return why;
}

static void define(ui_templates *t, const ui_wire_op *op) {
    ui_wire_define_template v;
    ui_wire_define_template_read(op->payload, op->payload_size, &v);
    ui_wire_define_template_tail tail;
    ui_wire_define_template_tail_of(op->payload, &v, &tail);
    const char *why = ui_wire_define_template_check(&v);
    bool *root = calloc(v.node_count ? v.node_count : 1, sizeof *root);
    uint8_t *ops = malloc(v.ops_bytes ? v.ops_bytes : 1);
    if (!why && (!root || !ops)) why = "out of memory";
    if (!why) why = check_ops(tail.ops, v.ops_bytes, v.node_count, root);
    ui_template *e = why ? NULL : insert(t, v.id);
    if (!why && !e) why = "out of memory";
    if (why) {
        free(root);
        free(ops);
        t->stats.dropped++;
        UI_DEBUG_REPORT(t->log, MODULE, "dropped define_template %u: %s", v.id, why);
        return;
    }
    free_entry(e);
    memcpy(ops, tail.ops, v.ops_bytes);
    *e = (ui_template){.id = v.id, .node_count = v.node_count, .ops_size = v.ops_bytes, .ops = ops, .root = root};
    t->stats.defined++;
}

static void forget(ui_templates *t, const ui_wire_op *op) {
    ui_wire_destroy_template v;
    ui_wire_destroy_template_read(op->payload, op->payload_size, &v);
    ui_template *e = v.id ? find(t, v.id) : NULL;
    if (!e) return;
    free_entry(e);
    e->id = TOMBSTONE;
    t->count--;
    t->stats.destroyed++;
}

// ---- Instances ----------------------------------------------------------------------------------

// Appends `op` (opcode, payload) to `b`. False when the buffer is full or memory short.
static bool copy_op(ui_wire_buf *b, uint16_t opcode, const uint8_t *payload, uint32_t payload_size) {
    uint8_t *p = ui_wire_buf_op(b, opcode, payload_size);
    if (!p) return false;
    memcpy(p, payload, payload_size);
    return true;
}

static ui_handle local_to(const uint8_t *p, const ui_wire_template_node *nodes) {
    return nodes[ui_wire_get_u32(p)].node;
}

// Appends the instance's ops; on failure the buffer is rolled back to where it was.
static bool instantiate(ui_templates *t, ui_wire_buf *b, const ui_wire_op *op) {
    ui_wire_instantiate_template v;
    ui_wire_instantiate_template_read(op->payload, op->payload_size, &v);
    const char *why = ui_wire_instantiate_template_check(&v);
    ui_template *e = why ? NULL : find(t, v.template);
    if (!why && !e) why = "no such template";
    if (!why && v.count != e->node_count) why = "its handles don't match the template's nodes";
    if (why) {
        t->stats.dropped++;
        UI_DEBUG_REPORT(t->log, MODULE, "dropped instantiate_template %u: %s", v.template, why);
        return true;
    }
    ui_wire_instantiate_template_tail tail;
    ui_wire_instantiate_template_tail_of(op->payload, &v, &tail);
    ui_wire_template_node *nodes = malloc(v.count * sizeof *nodes);
    if (!nodes) return false;
    for (uint32_t i = 0; i < v.count; i++)
        ui_wire_template_node_read(tail.nodes + i * UI_WIRE_TEMPLATE_NODE_SIZE, UI_WIRE_TEMPLATE_NODE_SIZE, &nodes[i]);
    uint32_t mark_size = b->size, mark_ops = b->op_count;
    bool ok = true;
    uint8_t scratch[UI_WIRE_MAX_OP_SIZE];
    for (uint32_t at = 0; ok && at < e->ops_size;) {
        uint16_t opcode = ui_wire_get_u16(e->ops + at), op_size = ui_wire_get_u16(e->ops + at + 2);
        uint32_t payload_size = op_size - UI_WIRE_OP_HEADER_SIZE;
        memcpy(scratch, e->ops + at + UI_WIRE_OP_HEADER_SIZE, payload_size);
        uint32_t node = ui_wire_get_u32(scratch);
        ui_wire_put_handle(scratch, local_to(scratch, nodes));
        if (opcode == UI_OP_CREATE_NODE) {
            bool is_root = e->root[node];
            ui_wire_put_handle(scratch + 8, is_root ? v.parent : local_to(scratch + 8, nodes));
            ok = copy_op(b, opcode, scratch, payload_size);
            // A root goes before `before` right away, as the equivalent create_node and
            // move_node would put it.
            if (ok && is_root && !ui_handle_is_null(v.before))
                ok = ui_wire_move_node_put(b, &(ui_wire_move_node){nodes[node].node, v.parent, v.before}) == UI_OK;
        } else {
            ok = copy_op(b, opcode, scratch, payload_size);
        }
        at += op_size;
    }
    free(nodes);
    if (!ok) {
        b->size = mark_size;
        b->op_count = mark_ops;
        t->stats.dropped++;
        UI_ERROR(t->log, MODULE, "dropped instantiate_template %u: the buffer would pass 64 MiB, or memory is short",
                 v.template);
        return true;
    }
    t->stats.instances++;
    return true;
}

ui_result ui_templates_expand(ui_templates *t, const uint8_t *data, size_t size, uint8_t **out, size_t *out_size) {
    *out = NULL;
    *out_size = 0;
    ui_wire_reader r;
    ui_wire_op op;
    if (ui_wire_validate_commands(data, size) != UI_OK) return UI_OK;
    // Most buffers hold no template op: leave them as they are.
    bool instances = false, any = false;
    ui_wire_reader_init(&r, UI_WIRE_MAGIC_CMD, data, size);
    while (ui_wire_reader_next(&r, &op) == UI_OK) {
        if (op.opcode == UI_OP_INSTANTIATE_TEMPLATE) instances = true;
        any |= op.opcode == UI_OP_DEFINE_TEMPLATE || op.opcode == UI_OP_DESTROY_TEMPLATE || instances;
    }
    if (!any) return UI_OK;
    ui_wire_buf b;
    ui_wire_buf_init(&b, UI_WIRE_MAGIC_CMD);
    ui_wire_reader_init(&r, UI_WIRE_MAGIC_CMD, data, size);
    while (ui_wire_reader_next(&r, &op) == UI_OK) {
        switch (op.opcode) {
        case UI_OP_DEFINE_TEMPLATE: define(t, &op); break;
        case UI_OP_DESTROY_TEMPLATE: forget(t, &op); break;
        case UI_OP_INSTANTIATE_TEMPLATE:
            if (!instantiate(t, &b, &op)) goto oom;
            continue; // replaced by its ops
        default: break;
        }
        if (instances && !copy_op(&b, op.opcode, op.payload, op.payload_size)) goto oom;
    }
    if (!instances) return UI_OK;
    *out = ui_wire_buf_finish(&b, out_size);
    if (!*out) {
        // Every op was an instance that was dropped: an empty buffer, as valid as any.
        ui_wire_buf_free(&b);
        uint8_t *empty = calloc(1, UI_WIRE_HEADER_SIZE);
        if (!empty) return UI_ERR_OUT_OF_MEMORY;
        ui_wire_put_u32(empty, UI_WIRE_MAGIC_CMD);
        ui_wire_put_u16(empty + 4, UI_WIRE_VERSION);
        ui_wire_put_u32(empty + 8, UI_WIRE_HEADER_SIZE);
        *out = empty;
        *out_size = UI_WIRE_HEADER_SIZE;
    }
    return UI_OK;
oom:
    ui_wire_buf_free(&b);
    return UI_ERR_OUT_OF_MEMORY;
}
