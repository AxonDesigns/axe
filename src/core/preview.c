#include "core/preview.h"

#include <stdlib.h>
#include <string.h>

#include "core/text_op.h"
#include "wire/gen/wire_gen.h"

// One node as the preview knows it: what the buffer does to it (when `over`), and the text
// values computed for it after the buffer (memo).
typedef struct pv_node {
    uint32_t slot;            // UI_NO_SLOT: an empty map entry
    bool over;                // the buffer touches it; the fields below replace the scene's
    bool exists;
    uint32_t generation;      // its node's, or the last its slot had (a new one must be higher)
    uint32_t parent, root;    // parent slot (UI_NO_SLOT: at the root `root`)
    bool styles_set, overrides_set, state_set;
    uint32_t *styles;
    uint32_t style_count;
    ui_style_entry *overrides;
    uint32_t override_count, override_capacity;
    uint8_t state;
    // The buffer's set_text ops for it, in order (each with a reader after it, where its
    // continuations start), and what the last that applies decodes to, when needed.
    struct pv_text {
        ui_wire_op op;
        ui_wire_reader after;
    } *texts;
    uint32_t text_count, text_capacity;
    bool text_decoded;
    ui_text_source *text;
    // Memo.
    bool done, alive;
    bool differs;             // its text values differ from the scene's
    ui_text_ctx ctx;
    ui_text_values values;    // what its children inherit
} pv_node;

struct ui_preview {
    const ui_scene *scene;
    ui_styles over;           // the buffer's definitions and tokens; the preview's strings
    uint32_t *gone_defs, *gone_tokens;
    uint32_t gone_def_count, gone_def_capacity, gone_token_count, gone_token_capacity;
    uint32_t *changed_defs, *changed_tokens; // directly or through tokens
    uint32_t changed_def_count, changed_def_capacity, changed_token_count, changed_token_capacity;
    pv_node *nodes;           // open addressing by slot
    uint32_t node_capacity, node_count;
    bool global;
    bool failed;              // out of memory: every answer is "unknown"
    const ui_style_entry **list; // cascade scratch
    uint32_t list_capacity;
    uint32_t *path;           // evaluation scratch
    uint32_t path_capacity;
};

// ---- Small sets -----------------------------------------------------------------------

static bool has(const uint32_t *set, uint32_t n, uint32_t id) {
    for (uint32_t i = 0; i < n; i++) {
        if (set[i] == id) return true;
    }
    return false;
}

static bool add(ui_preview *p, uint32_t **set, uint32_t *n, uint32_t *cap, uint32_t id) {
    if (has(*set, *n, id)) return true;
    if (ui_grow((void **)set, cap, *n + 1, sizeof **set) != UI_OK) return !(p->failed = true);
    (*set)[(*n)++] = id;
    return true;
}

static void drop(uint32_t *set, uint32_t *n, uint32_t id) {
    for (uint32_t i = 0; i < *n; i++) {
        if (set[i] == id) {
            set[i] = set[--*n];
            return;
        }
    }
}

// ---- Nodes ----------------------------------------------------------------------------

static uint32_t hash_slot(uint32_t slot) {
    slot *= 0x9E3779B1u;
    return slot ^ (slot >> 16);
}

static pv_node *find(const ui_preview *p, uint32_t slot) {
    if (!p->node_capacity) return NULL;
    uint32_t mask = p->node_capacity - 1;
    for (uint32_t i = hash_slot(slot) & mask;; i = (i + 1) & mask) {
        pv_node *e = &p->nodes[i];
        if (e->slot == slot) return e;
        if (e->slot == UI_NO_SLOT) return NULL;
    }
}

// The node's entry, made when missing (from the scene's node, or as a slot nothing uses).
static pv_node *get(ui_preview *p, uint32_t slot) {
    pv_node *e = find(p, slot);
    if (e) return e;
    if ((p->node_count + 1) * 2 > p->node_capacity) {
        uint32_t cap = p->node_capacity ? p->node_capacity * 2 : 64;
        pv_node *old = p->nodes;
        uint32_t old_cap = p->node_capacity;
        if (!(p->nodes = malloc(cap * sizeof *p->nodes))) {
            p->nodes = old;
            p->failed = true;
            return NULL;
        }
        for (uint32_t i = 0; i < cap; i++) p->nodes[i].slot = UI_NO_SLOT;
        p->node_capacity = cap;
        for (uint32_t i = 0; i < old_cap; i++) {
            if (old[i].slot == UI_NO_SLOT) continue;
            uint32_t mask = cap - 1, k = hash_slot(old[i].slot) & mask;
            while (p->nodes[k].slot != UI_NO_SLOT) k = (k + 1) & mask;
            p->nodes[k] = old[i];
        }
        free(old);
    }
    uint32_t mask = p->node_capacity - 1, k = hash_slot(slot) & mask;
    while (p->nodes[k].slot != UI_NO_SLOT) k = (k + 1) & mask;
    e = &p->nodes[k];
    memset(e, 0, sizeof *e);
    e->slot = slot;
    e->parent = UI_NO_SLOT;
    p->node_count++;
    const ui_scene *s = p->scene;
    if (slot < s->handles.used) e->generation = s->handles.generations[slot];
    if (slot < s->handles.used && s->handles.alive[slot]) {
        const ui_node *n = &s->nodes[slot];
        e->exists = true;
        e->parent = n->parent;
        e->root = n->root;
    }
    return e;
}

// Takes over the scene node's styling before the buffer changes it.
static pv_node *touch(ui_preview *p, uint32_t slot) {
    pv_node *e = get(p, slot);
    if (e) e->over = true;
    return e;
}

static bool alive(ui_preview *p, ui_handle h) {
    if (ui_handle_kind_of(h) != UI_HANDLE_NODE) return false;
    pv_node *e = get(p, ui_handle_slot(h));
    if (!e || !e->exists || e->generation != h.generation) return false;
    // A destroyed ancestor took it with it.
    uint32_t steps = 0;
    for (uint32_t up = e->parent; up != UI_NO_SLOT; up = e->parent) {
        if (!(e = get(p, up)) || !e->exists || ++steps > p->scene->handles.used + p->node_count) return false;
    }
    return true;
}

// The surface a node is under after the buffer (its top ancestor's; a move to another
// surface takes the subtree along).
static uint32_t root_of(ui_preview *p, uint32_t slot) {
    pv_node *e = get(p, slot);
    for (uint32_t steps = 0; e && e->parent != UI_NO_SLOT && steps <= p->scene->handles.used + p->node_count; steps++)
        e = get(p, e->parent);
    return e ? e->root : UI_MAX_SURFACES;
}

static void copy_overrides(ui_preview *p, pv_node *e) {
    if (e->overrides_set) return;
    e->overrides_set = true;
    const ui_scene *s = p->scene;
    if (e->slot >= s->handles.used || !s->handles.alive[e->slot]) return;
    const ui_node *n = &s->nodes[e->slot];
    if (!n->override_count) return;
    if (ui_grow((void **)&e->overrides, &e->override_capacity, n->override_count, sizeof *e->overrides) != UI_OK) {
        p->failed = true;
        return;
    }
    memcpy(e->overrides, n->overrides, n->override_count * sizeof *e->overrides);
    e->override_count = n->override_count;
}

static void set_override(ui_preview *p, pv_node *e, const ui_style_entry *entry) {
    for (uint32_t i = 0; i < e->override_count; i++) {
        if (e->overrides[i].prop != entry->prop) continue;
        if (entry->value.kind == UI_VALUE_UNSET) e->overrides[i] = e->overrides[--e->override_count];
        else e->overrides[i] = *entry;
        return;
    }
    if (entry->value.kind == UI_VALUE_UNSET) return;
    if (ui_grow((void **)&e->overrides, &e->override_capacity, e->override_count + 1, sizeof *e->overrides) != UI_OK) {
        p->failed = true;
        return;
    }
    e->overrides[e->override_count++] = *entry;
}

// True if an entry can change a text value: a text property, or inherit/initial (which may
// copy one), or a token (which may resolve to anything the property takes).
static bool text_entry(const ui_style_entry *e) {
    switch (e->prop) {
    case UI_STYLE_TEXT_COLOR:
    case UI_STYLE_LINE_HEIGHT:
    case UI_STYLE_TEXT_ALIGN:
    case UI_STYLE_FONT_FAMILY:
    case UI_STYLE_FONT_SIZE:
    case UI_STYLE_FONT_WEIGHT:
    case UI_STYLE_FONT_SLANT:
    case UI_STYLE_LANGUAGE:
    case UI_STYLE_DIRECTION: return true;
    default: return false;
    }
}

static bool def_has_text(const ui_style_def *d, bool state_blocks_only) {
    for (uint32_t b = 0; d && b < d->block_count; b++) {
        if (state_blocks_only && !d->blocks[b].states) continue;
        for (uint32_t k = 0; k < d->blocks[b].count; k++) {
            if (text_entry(&d->entries[d->blocks[b].first + k])) return true;
        }
    }
    return false;
}

static ui_style_view view_of(const ui_preview *p) {
    return (ui_style_view){&p->scene->styles, &p->over, p->gone_defs, p->gone_tokens,
                           p->gone_def_count, p->gone_token_count};
}

// The node's style list after the buffer.
static void styles_of(const ui_preview *p, const pv_node *e, const uint32_t **ids, uint32_t *count) {
    const ui_scene *s = p->scene;
    if (e->styles_set) {
        *ids = e->styles;
        *count = e->style_count;
    } else if (e->slot < s->handles.used && s->handles.alive[e->slot]) {
        *ids = s->nodes[e->slot].styles;
        *count = s->nodes[e->slot].style_count;
    } else {
        *ids = NULL;
        *count = 0;
    }
}

// ---- Reading the buffer ------------------------------------------------------------------

#define READ_OP(type, var)                                                                  \
    ui_wire_##type var;                                                                     \
    ui_wire_##type##_read(op->payload, op->payload_size, &var)

static void read_op(ui_preview *p, const ui_wire_op *op, const ui_wire_reader *after) {
    const uint8_t *q = op->payload;
    const ui_scene *s = p->scene;
    switch (op->opcode) {
    case UI_OP_CREATE_NODE: {
        READ_OP(create_node, o);
        ui_handle h = o.node, parent = o.parent;
        if (ui_handle_kind_of(h) != UI_HANDLE_NODE) return;
        pv_node *e = get(p, ui_handle_slot(h));
        if (!e) return;
        // As ui_handle_table_claim: a live slot, or a generation not above its last, drops it.
        // (A node under a destroyed one went with it: its slot is free.)
        if (h.generation <= e->generation || ui_handle_slot(h) >= UI_HANDLE_MAX_SLOTS ||
            alive(p, (ui_handle){h.index, e->generation}))
            return;
        uint32_t root = UI_MAX_SURFACES, parent_slot = UI_NO_SLOT;
        if (ui_handle_kind_of(parent) == UI_HANDLE_SURFACE) {
            root = ui_scene_root_index(s, parent);
        } else if (alive(p, parent)) {
            parent_slot = ui_handle_slot(parent);
            root = root_of(p, parent_slot);
        }
        if (root == UI_MAX_SURFACES) return;
        if (!(e = touch(p, ui_handle_slot(h)))) return;
        if (e->styles_set) free(e->styles);
        free(e->overrides);
        free(e->texts);
        ui_text_source_release(e->text);
        *e = (pv_node){.slot = e->slot, .over = true, .exists = true, .generation = h.generation,
                       .parent = parent_slot, .root = root, .styles_set = true, .overrides_set = true,
                       .state_set = true};
        return;
    }
    case UI_OP_DESTROY_NODE: {
        READ_OP(destroy_node, o);
        ui_handle h = o.node;
        if (alive(p, h)) touch(p, ui_handle_slot(h))->exists = false;
        return;
    }
    case UI_OP_MOVE_NODE: {
        READ_OP(move_node, o);
        ui_handle h = o.node, parent = o.parent;
        if (!alive(p, h)) return;
        uint32_t slot = ui_handle_slot(h), root = UI_MAX_SURFACES, parent_slot = UI_NO_SLOT;
        if (ui_handle_kind_of(parent) == UI_HANDLE_SURFACE) {
            root = ui_scene_root_index(s, parent);
        } else if (alive(p, parent)) {
            parent_slot = ui_handle_slot(parent);
            for (uint32_t up = parent_slot; up != UI_NO_SLOT; up = get(p, up)->parent)
                if (up == slot) return; // under itself: dropped
            root = root_of(p, parent_slot);
        }
        if (root == UI_MAX_SURFACES) return;
        // As the scene: a `before` must be another node under the new parent.
        ui_handle before = o.before;
        if (!ui_handle_is_null(before)) {
            if (!alive(p, before) || ui_handle_slot(before) == slot) return;
            if (get(p, ui_handle_slot(before))->parent != parent_slot || root_of(p, ui_handle_slot(before)) != root)
                return;
        }
        pv_node *e = touch(p, slot);
        e->parent = parent_slot;
        e->root = root;
        p->global = true;
        return;
    }
    case UI_OP_SET_STYLES: {
        ui_wire_set_styles o;
        ui_wire_set_styles_read(q, ui_wire_set_styles_header_size(q), &o);
        ui_wire_set_styles_tail t;
        ui_wire_set_styles_tail_of(q, &o, &t);
        ui_handle h = o.node;
        if (!alive(p, h)) return;
        uint32_t count = o.count;
        // As the scene: a 0 id, or more than 255, drops the op.
        if (count > UINT8_MAX) return;
        for (uint32_t i = 0; i < count; i++)
            if (!ui_wire_set_styles_ids_at(&t, i)) return;
        pv_node *e = touch(p, ui_handle_slot(h));
        const uint32_t *old;
        uint32_t old_count;
        styles_of(p, e, &old, &old_count);
        ui_style_view v = view_of(p);
        for (uint32_t i = 0; !p->global && i < old_count; i++)
            p->global = def_has_text(ui_style_view_find(&v, old[i]), false);
        uint32_t *list = malloc((count ? count : 1) * sizeof *list);
        if (!list) {
            p->failed = true;
            return;
        }
        for (uint32_t i = 0; i < count; i++) {
            list[i] = ui_wire_set_styles_ids_at(&t, i);
            if (!p->global) p->global = def_has_text(ui_style_view_find(&v, list[i]), false);
        }
        if (e->styles_set) free(e->styles);
        e->styles = list;
        e->style_count = count;
        e->styles_set = true;
        return;
    }
    case UI_OP_SET_PROPS: {
        ui_wire_set_props o;
        ui_wire_set_props_read(q, ui_wire_set_props_header_size(q), &o);
        ui_wire_set_props_tail t;
        ui_wire_set_props_tail_of(q, &o, &t);
        ui_handle h = o.node;
        if (!alive(p, h)) return;
        uint32_t count = o.entry_count, n;
        ui_style_entry local[16], *entries = count <= 16 ? local : malloc(count * sizeof *entries);
        if (!entries) {
            p->failed = true;
            return;
        }
        if (ui_style_decode(&p->over, t.entries, count, true, entries, &n)) {
            pv_node *e = touch(p, ui_handle_slot(h));
            copy_overrides(p, e);
            for (uint32_t i = 0; i < n; i++) {
                if (text_entry(&entries[i])) p->global = true;
                set_override(p, e, &entries[i]);
            }
        }
        if (entries != local) free(entries);
        return; // the strings stay interned in `over` until the preview is destroyed
    }
    case UI_OP_SET_STATE: {
        READ_OP(set_state, o);
        ui_handle h = o.node;
        if (!alive(p, h)) return;
        pv_node *e = touch(p, ui_handle_slot(h));
        uint8_t state = e->state_set ? e->state : s->nodes[e->slot].state;
        uint32_t mask = o.mask, value = o.value;
        e->state = (uint8_t)((state & ~mask) | (value & mask));
        e->state_set = true;
        if (!p->global && e->state != state) {
            const uint32_t *ids;
            uint32_t count;
            styles_of(p, e, &ids, &count);
            ui_style_view v = view_of(p);
            for (uint32_t i = 0; !p->global && i < count; i++)
                p->global = def_has_text(ui_style_view_find(&v, ids[i]), true);
        }
        return;
    }
    case UI_OP_SET_TEXT: {
        ui_wire_set_text o;
        ui_wire_set_text_read(q, ui_wire_set_text_header_size(q), &o);
        ui_handle h = o.node;
        if (!alive(p, h)) return;
        pv_node *e = touch(p, ui_handle_slot(h));
        if (ui_grow((void **)&e->texts, &e->text_capacity, e->text_count + 1, sizeof *e->texts) != UI_OK) {
            p->failed = true;
            return;
        }
        e->texts[e->text_count++] = (struct pv_text){*op, *after};
        e->text_decoded = false;
        ui_text_source_release(e->text);
        e->text = NULL;
        // Its values reach its children: only a node with children makes it global.
        if (e->slot < s->handles.used && s->handles.alive[e->slot] && s->nodes[e->slot].first_child != UI_NO_SLOT)
            p->global = true;
        return;
    }
    case UI_OP_DEFINE_STYLE: {
        uint32_t id;
        if (ui_styles_define(&p->over, q, &id)) {
            drop(p->gone_defs, &p->gone_def_count, id);
            add(p, &p->changed_defs, &p->changed_def_count, &p->changed_def_capacity, id);
            p->global = true;
        }
        return;
    }
    case UI_OP_DESTROY_STYLE: {
        READ_OP(destroy_style, o);
        uint32_t id = o.id;
        ui_styles_destroy(&p->over, id);
        add(p, &p->gone_defs, &p->gone_def_count, &p->gone_def_capacity, id);
        add(p, &p->changed_defs, &p->changed_def_count, &p->changed_def_capacity, id);
        p->global = true;
        return;
    }
    case UI_OP_SET_TOKEN: {
        READ_OP(set_token, o);
        ui_wire_set_token_tail t;
        ui_wire_set_token_tail_of(q, &o, &t);
        ui_wire_style_entry entry;
        ui_wire_style_entry_read(t.entry, UI_WIRE_STYLE_ENTRY_SIZE, &entry);
        uint32_t id;
        if (!ui_styles_set_token(&p->over, q, &id)) return;
        if (entry.kind == UI_VALUE_UNSET) add(p, &p->gone_tokens, &p->gone_token_count, &p->gone_token_capacity, id);
        else drop(p->gone_tokens, &p->gone_token_count, id);
        add(p, &p->changed_tokens, &p->changed_token_count, &p->changed_token_capacity, id);
        p->global = true;
        return;
    }
    default: return;
    }
}

// Tokens whose chain reaches a changed token changed too; styles using a changed token too.
static void close_changes(ui_preview *p) {
    const ui_styles *tables[2] = {&p->scene->styles, &p->over};
    for (bool grew = p->changed_token_count > 0; grew && !p->failed;) {
        grew = false;
        for (int t = 0; t < 2; t++) {
            for (uint32_t i = 0; i < tables[t]->token_capacity; i++) {
                const ui_token *tok = &tables[t]->tokens[i];
                if (!tok->id || tok->value.kind != UI_VALUE_TOKEN ||
                    has(p->changed_tokens, p->changed_token_count, tok->id) ||
                    !has(p->changed_tokens, p->changed_token_count, tok->value.token))
                    continue;
                add(p, &p->changed_tokens, &p->changed_token_count, &p->changed_token_capacity, tok->id);
                grew = true;
            }
        }
    }
    for (int t = 0; t < 2 && p->changed_token_count; t++) {
        for (uint32_t i = 0; i < tables[t]->def_capacity; i++) {
            const ui_style_def *d = &tables[t]->defs[i];
            for (uint32_t k = 0; d->id && k < d->token_count; k++) {
                if (!has(p->changed_tokens, p->changed_token_count, d->tokens[k])) continue;
                add(p, &p->changed_defs, &p->changed_def_count, &p->changed_def_capacity, d->id);
                break;
            }
        }
    }
}

ui_preview *ui_preview_create(const ui_scene *s, const ui_preview_buffer *buffers, uint32_t count) {
    ui_preview *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->scene = s;
    ui_styles_init(&p->over);
    for (uint32_t i = 0; i < count && !p->failed; i++) {
        ui_wire_reader r;
        ui_wire_op op;
        if (ui_wire_reader_init(&r, UI_WIRE_MAGIC_CMD, buffers[i].data, buffers[i].size) != UI_OK) continue;
        while (!p->failed && ui_wire_reader_next(&r, &op) == UI_OK) read_op(p, &op, &r);
    }
    close_changes(p);
    if (p->failed) {
        ui_preview_destroy(p);
        return NULL;
    }
    return p;
}

void ui_preview_destroy(ui_preview *p) {
    if (!p) return;
    for (uint32_t i = 0; i < p->node_capacity; i++) {
        pv_node *e = &p->nodes[i];
        if (e->slot == UI_NO_SLOT) continue;
        if (e->styles_set) free(e->styles);
        free(e->overrides);
        free(e->texts);
        ui_text_source_release(e->text);
    }
    free(p->nodes);
    ui_styles_free(&p->over);
    free(p->gone_defs);
    free(p->gone_tokens);
    free(p->changed_defs);
    free(p->changed_tokens);
    free(p->list);
    free(p->path);
    free(p);
}

bool ui_preview_global(const ui_preview *p) { return p->global; }

bool ui_preview_sets_text(const ui_preview *p, uint32_t slot) {
    const pv_node *e = find(p, slot);
    return e && e->text_count;
}

// ---- Evaluating ------------------------------------------------------------------------

// The node uses a style or token the buffer changes.
static bool uses_changes(const ui_preview *p, const pv_node *e) {
    if (!p->changed_def_count && !p->changed_token_count) return false;
    const uint32_t *ids;
    uint32_t count;
    styles_of(p, e, &ids, &count);
    for (uint32_t i = 0; i < count; i++) {
        if (has(p->changed_defs, p->changed_def_count, ids[i])) return true;
    }
    const ui_scene *s = p->scene;
    const ui_style_entry *o = e->overrides_set ? e->overrides : s->nodes[e->slot].overrides;
    uint32_t n = e->overrides_set ? e->override_count : s->nodes[e->slot].override_count;
    for (uint32_t i = 0; i < n; i++) {
        if (o[i].value.kind == UI_VALUE_TOKEN && has(p->changed_tokens, p->changed_token_count, o[i].value.token))
            return true;
    }
    return false;
}

// The node's set_text after the buffer. The buffer's own is decoded exactly as applying it
// will be (a set_text that would be dropped leaves the old one), when first needed: only a
// node whose children are asked about needs it.
static const ui_text_source *base_of(ui_preview *p, pv_node *e) {
    const ui_scene *s = p->scene;
    bool scene_node = e->slot < s->handles.used && s->handles.alive[e->slot] &&
                      e->generation == s->handles.generations[e->slot];
    if (e->text_count && !e->text_decoded) {
        e->text_decoded = true;
        for (uint32_t i = e->text_count; i-- && !e->text;) {
            ui_text_op t;
            ui_wire_reader r = e->texts[i].after;
            ui_text_op_decode(&e->texts[i].op, &r, &t);
            if (t.out_of_memory) p->failed = true;
            e->text = t.source; // the reference moves here (NULL: dropped, try the one before)
        }
    }
    if (e->text) return e->text;
    return scene_node ? s->nodes[e->slot].text_base : NULL;
}

// Computes one node whose parent (if any) is done.
static void evaluate(ui_preview *p, pv_node *e) {
    const ui_scene *s = p->scene;
    e->done = true;
    e->alive = false;
    if (!e->exists) return;
    const ui_text_values *parent;
    bool parent_differs = false;
    if (e->parent == UI_NO_SLOT) {
        if (e->root >= UI_MAX_SURFACES) return;
        parent = &s->roots[e->root].computed.text;
    } else {
        const pv_node *up = find(p, e->parent);
        if (!up || !up->alive) return;
        parent = &up->values;
        parent_differs = up->differs;
    }
    e->alive = true;
    bool scene_node = e->slot < s->handles.used && s->handles.alive[e->slot] &&
                      s->handles.generations[e->slot] == e->generation;
    if (scene_node && !e->over && !parent_differs && !uses_changes(p, e)) {
        e->ctx = ui_scene_text_ctx(s, e->slot);
        e->values = s->nodes[e->slot].computed.text;
        e->differs = false;
        return;
    }
    // Its cascade after the buffer, computed on its parent's values after the buffer.
    const uint32_t *ids;
    uint32_t count, n = 0;
    styles_of(p, e, &ids, &count);
    uint8_t state = e->state_set ? e->state : scene_node ? s->nodes[e->slot].state : 0;
    const ui_style_entry *o = e->overrides_set ? e->overrides : scene_node ? s->nodes[e->slot].overrides : NULL;
    uint32_t on = e->overrides_set ? e->override_count : scene_node ? s->nodes[e->slot].override_count : 0;
    ui_style_view v = view_of(p);
    if (!ui_style_cascade(&v, ids, count, state, o, on, &p->list, &n, &p->list_capacity)) {
        p->failed = true;
        return;
    }
    ui_computed pc, c;
    ui_computed_init(&pc);
    pc.text = *parent;
    ui_computed_start(&c, &pc);
    uint32_t declared;
    ui_style_compute(&v, p->list, n, &pc, &c, &declared);
    e->ctx = (ui_text_ctx){c.text, declared};
    if (!ui_text_ctx_values(&e->ctx, base_of(p, e), &p->over, &e->values)) p->failed = true;
    e->differs = !scene_node || !ui_text_values_eq(&e->values, &s->nodes[e->slot].computed.text);
}

bool ui_preview_ctx(ui_preview *p, ui_handle node, ui_text_ctx *out, const ui_text_source **base) {
    if (p->failed || ui_handle_kind_of(node) != UI_HANDLE_NODE) return false;
    pv_node *e = get(p, ui_handle_slot(node));
    if (!e || !e->exists || e->generation != node.generation) return false;
    // The path up to the first node already computed (or the root), then down.
    uint32_t count = 0;
    for (pv_node *cur = e; cur && !cur->done;) {
        if (ui_grow((void **)&p->path, &p->path_capacity, count + 1, sizeof *p->path) != UI_OK ||
            count > p->scene->handles.used + p->node_count) {
            p->failed = true;
            return false;
        }
        p->path[count++] = cur->slot;
        if (!cur->exists || cur->parent == UI_NO_SLOT) break;
        cur = get(p, cur->parent);
        if (!cur) return false;
    }
    while (count && !p->failed) evaluate(p, find(p, p->path[--count]));
    e = find(p, ui_handle_slot(node));
    if (p->failed || !e->alive) return false;
    *out = e->ctx;
    if (base) *base = base_of(p, e);
    return true;
}
