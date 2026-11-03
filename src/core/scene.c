#include "core/scene.h"

#include <math.h>
#include <string.h>

#include "core/color.h"
#include "style/props.h"
#include "unicode/utf8.h"
#include "wire/wire.h"

#define MODULE "scene"

// Settle thresholds per spring, in the property's units (px, linear color, opacity).
static const double spring_epsilon[UI_SPRING_COUNT] = {0.01, 1e-4, 1e-4, 1e-4, 1e-4, 1e-4, 0.01, 1e-4, 1e-4, 0.01};
// Components per spring.
static const uint32_t spring_components[UI_SPRING_COUNT] = {4, 4, 4, 4, 4, 4, 4, 1, 4, 4};

// What the build keeps per ancestor level.
struct ui_build_level {
    uint32_t slot;   // the node whose children are being walked (UI_NO_SLOT: the root)
    float x, y;      // its origin, logical px from the surface's
    float clip[4];   // what its children are clipped to (physical px: x0, y0, x1, y1)
    bool moving;     // it or an ancestor moves: its children draw unsnapped
    bool layer;      // it opened a layer, closed after its children
};

void ui_scene_init(ui_scene *s, const ui_logger *log) {
    memset(s, 0, sizeof *s);
    s->log = log;
    ui_handle_table_init(&s->handles, UI_HANDLE_NODE);
    ui_styles_init(&s->styles);
}

static void free_node(ui_scene *s, ui_node *n) {
    if (n->text) {
        ui_paragraph_free(n->text);
        free(n->text);
    }
    for (uint32_t i = 0; i < n->override_count; i++) ui_styles_release_value(&s->styles, &n->overrides[i].value);
    ui_styles_release(&s->styles, n->text_family);
    ui_styles_release(&s->styles, n->text_language);
    ui_text_source_release(n->text_base);
    n->text_base = NULL;
    n->text_keyed = false;
    free(n->anim);
    free(n->styles);
    free(n->overrides);
    free(n->cascade);
    n->text = NULL;
    n->anim = NULL;
    n->styles = NULL;
    n->overrides = NULL;
    n->cascade = NULL;
    n->text_family = n->text_language = NULL;
}

void ui_scene_destroy(ui_scene *s) {
    for (uint32_t slot = 0; slot < s->handles.used; slot++) {
        if (s->handles.alive[slot]) free_node(s, &s->nodes[slot]);
    }
    ui_handle_table_destroy(&s->handles);
    ui_styles_free(&s->styles);
    free(s->nodes);
    free(s->style_dirty);
    free(s->scratch);
    free(s->levels);
    for (uint32_t i = 0; i < s->offer_count; i++) {
        ui_text_source_release(s->offers[i].base);
        ui_text_source_release(s->offers[i].source);
        ui_shaped_paragraph_free(&s->offers[i].shaped);
    }
    free(s->offers);
    free(s->requests);
    memset(s, 0, sizeof *s);
}

static ui_scene_root *find_root(ui_scene *s, ui_handle surface) {
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        if (!ui_handle_is_null(s->roots[i].surface) && ui_handle_eq(s->roots[i].surface, surface))
            return &s->roots[i];
    }
    return NULL;
}


uint32_t ui_scene_root_index(const ui_scene *s, ui_handle surface) {
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        if (!ui_handle_is_null(s->roots[i].surface) && ui_handle_eq(s->roots[i].surface, surface)) return i;
    }
    return UI_MAX_SURFACES;
}

ui_result ui_scene_add_root(ui_scene *s, ui_handle surface) {
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        if (ui_handle_is_null(s->roots[i].surface)) {
            s->roots[i] = (ui_scene_root){.surface = surface, .first_child = UI_NO_SLOT,
                                          .last_child = UI_NO_SLOT, .scale = 1.f, .layout_dirty = true};
            ui_computed_init(&s->roots[i].computed);
            return UI_OK;
        }
    }
    return UI_ERR_FULL;
}

static ui_node *node_of(ui_scene *s, ui_handle h) {
    return ui_handle_table_valid(&s->handles, h) ? &s->nodes[ui_handle_slot(h)] : NULL;
}

static ui_handle handle_of(const ui_scene *s, uint32_t slot) {
    return (ui_handle){((uint32_t)UI_HANDLE_NODE << UI_HANDLE_SLOT_BITS) | slot,
                       s->handles.generations[slot]};
}

static void unlink(ui_scene *s, uint32_t slot) {
    ui_node *n = &s->nodes[slot];
    uint32_t *first, *last;
    if (n->parent == UI_NO_SLOT) {
        first = &s->roots[n->root].first_child;
        last = &s->roots[n->root].last_child;
    } else {
        first = &s->nodes[n->parent].first_child;
        last = &s->nodes[n->parent].last_child;
    }
    if (n->prev_sibling != UI_NO_SLOT) s->nodes[n->prev_sibling].next_sibling = n->next_sibling;
    else *first = n->next_sibling;
    if (n->next_sibling != UI_NO_SLOT) s->nodes[n->next_sibling].prev_sibling = n->prev_sibling;
    else *last = n->prev_sibling;
}

// Releases a node and its whole subtree. The traversal stack is reserved up front (a subtree
// never has more nodes than are alive), so the destroy either fully happens or not at all.
static void parent_layout_dirty(ui_scene *s, uint32_t slot);

static bool destroy_subtree(ui_scene *s, uint32_t slot) {
    if (ui_grow((void **)&s->scratch, &s->scratch_capacity, s->handles.live + 1,
                sizeof *s->scratch) != UI_OK) {
        UI_ERROR(s->log, MODULE, "out of memory destroying a subtree");
        return false;
    }
    parent_layout_dirty(s, slot);
    unlink(s, slot);
    uint32_t count = 0;
    s->scratch[count++] = slot;
    while (count) {
        uint32_t cur = s->scratch[--count];
        for (uint32_t c = s->nodes[cur].first_child; c != UI_NO_SLOT; c = s->nodes[c].next_sibling)
            s->scratch[count++] = c;
        free_node(s, &s->nodes[cur]);
        ui_handle_table_release(&s->handles, handle_of(s, cur));
    }
    return true;
}

void ui_scene_remove_root(ui_scene *s, ui_handle surface) {
    ui_scene_root *root = find_root(s, surface);
    if (!root) return;
    while (root->first_child != UI_NO_SLOT) {
        if (!destroy_subtree(s, root->first_child)) break;
    }
    root->surface = (ui_handle){0};
}

static bool finite_n(const float *v, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (!isfinite(v[i])) return false;
    }
    return true;
}

// Checks the framing of every op without applying anything. A known op's payload must hold
// at least its original fields and, for variable-length ops, the data its counts describe;
// bytes past that come from a newer writer and are ignored.
ui_result ui_scene_validate(const uint8_t *data, size_t size) {
    ui_wire_reader r;
    ui_result res = ui_wire_reader_init(&r, UI_WIRE_MAGIC_CMD, data, size);
    if (res != UI_OK) return res;
    ui_wire_op op;
    while ((res = ui_wire_reader_next(&r, &op)) == UI_OK) {
        uint64_t expected;
        switch (op.opcode) {
        case UI_OP_CREATE_NODE: expected = UI_WIRE_SIZE_CREATE_NODE; break;
        case UI_OP_DESTROY_NODE: expected = UI_WIRE_SIZE_DESTROY_NODE; break;
        case UI_OP_SET_RECT: expected = UI_WIRE_SIZE_SET_RECT; break;
        case UI_OP_SET_CORNER_RADIUS: expected = UI_WIRE_SIZE_SET_CORNER_RADIUS; break;
        case UI_OP_SET_COLOR: expected = UI_WIRE_SIZE_SET_COLOR; break;
        case UI_OP_SET_TIMER: expected = UI_WIRE_SIZE_SET_TIMER; break;
        case UI_OP_SET_CLIP: expected = UI_WIRE_SIZE_SET_CLIP; break;
        case UI_OP_LOAD_FONT: {
            if (op.payload_size < UI_WIRE_SIZE_LOAD_FONT_HEADER) return UI_ERR_MALFORMED;
            uint32_t header = ui_wire_load_font_header(op.payload);
            if (header < UI_WIRE_SIZE_LOAD_FONT_HEADER || header % 4) return UI_ERR_MALFORMED;
            expected = header + ui_wire_pad4(ui_wire_get_u16(op.payload + 4));
            break;
        }
        case UI_OP_START_SPRING:
            if (op.payload_size < UI_WIRE_SIZE_START_SPRING_BASE) return UI_ERR_MALFORMED;
            expected = UI_WIRE_SIZE_START_SPRING_BASE + 4u * ui_wire_get_u16(op.payload + 10);
            break;
        case UI_OP_SET_TEXT: {
            if (op.payload_size < UI_WIRE_MIN_SET_TEXT_HEADER) return UI_ERR_MALFORMED;
            const uint8_t *p = op.payload;
            uint32_t header = ui_wire_set_text_header(p), stride = ui_wire_set_text_span(p);
            uint32_t chunk = ui_wire_get_u16(p + 12), spans = ui_wire_get_u16(p + 16);
            uint32_t fam = ui_wire_get_u16(p + 20), ell = ui_wire_get_u16(p + 22);
            if (header < UI_WIRE_MIN_SET_TEXT_HEADER || header % 4 ||
                stride < UI_WIRE_SIZE_TEXT_SPAN || stride % 4 || fam % 4)
                return UI_ERR_MALFORMED;
            expected = header + (uint64_t)spans * stride + fam + ui_wire_pad4(ell) +
                       ui_wire_pad4(chunk);
            break;
        }
        case UI_OP_APPEND_TEXT: {
            if (op.payload_size < UI_WIRE_SIZE_APPEND_TEXT_HEADER) return UI_ERR_MALFORMED;
            uint32_t header = ui_wire_append_text_header(op.payload);
            if (header < UI_WIRE_SIZE_APPEND_TEXT_HEADER || header % 4) return UI_ERR_MALFORMED;
            expected = header + ui_wire_pad4(ui_wire_get_u16(op.payload + 8));
            break;
        }
        case UI_OP_DEFINE_STYLE:
            if (op.payload_size < UI_WIRE_SIZE_DEFINE_STYLE_HEADER ||
                !ui_style_walk_define(op.payload, op.payload_size))
                return UI_ERR_MALFORMED;
            continue;
        case UI_OP_DESTROY_STYLE: expected = UI_WIRE_SIZE_DESTROY_STYLE; break;
        case UI_OP_SET_STATE: expected = UI_WIRE_SIZE_SET_STATE; break;
        case UI_OP_MOVE_NODE: expected = UI_WIRE_SIZE_MOVE_NODE; break;
        case UI_OP_REQUEST_LAYOUT: expected = UI_WIRE_SIZE_REQUEST_LAYOUT; break;
        case UI_OP_SET_STYLES: {
            if (op.payload_size < UI_WIRE_SIZE_SET_STYLES_HEADER) return UI_ERR_MALFORMED;
            uint32_t header = ui_wire_set_styles_header(op.payload);
            if (header < UI_WIRE_SIZE_SET_STYLES_HEADER || header % 4) return UI_ERR_MALFORMED;
            expected = header + 4u * ui_wire_get_u16(op.payload + 8);
            break;
        }
        case UI_OP_SET_PROPS: {
            if (op.payload_size < UI_WIRE_SIZE_SET_PROPS_HEADER) return UI_ERR_MALFORMED;
            uint32_t header = ui_wire_set_props_header(op.payload), end;
            if (header < UI_WIRE_SIZE_SET_PROPS_HEADER || header % 4 || header > op.payload_size ||
                !ui_style_walk(op.payload + header, op.payload_size - header,
                               ui_wire_get_u16(op.payload + 10), &end))
                return UI_ERR_MALFORMED;
            continue;
        }
        case UI_OP_SET_TOKEN: {
            const uint32_t entry = UI_WIRE_SIZE_SET_TOKEN_HEADER + UI_WIRE_STYLE_ENTRY_HEADER;
            if (op.payload_size < entry || !op.payload[UI_WIRE_SIZE_SET_TOKEN_HEADER + 3])
                return UI_ERR_MALFORMED;
            expected = UI_WIRE_SIZE_SET_TOKEN_HEADER + 4u * op.payload[UI_WIRE_SIZE_SET_TOKEN_HEADER + 3];
            break;
        }
        default: continue; // unknown ops are skipped when applying
        }
        if (op.payload_size < expected) return UI_ERR_MALFORMED;
    }
    return res == UI_ERR_END ? UI_OK : res;
}

static bool drop(ui_scene *s, const char *what, ui_handle h) {
    s->stats.dropped++;
    UI_DEBUG_REPORT(s->log, MODULE, "dropped %s: handle %08x:%u is stale or invalid", what,
                    h.index, h.generation);
    (void)what;
    (void)h;
    return false;
}

// Queues a node for style resolution; with `cascade`, what applies to it changed too (its
// styles, state or overrides), not only the values its entries read.
static void mark_style_dirty(ui_scene *s, uint32_t slot, bool cascade) {
    ui_node *n = &s->nodes[slot];
    n->cascade_dirty |= cascade;
    if (n->style_dirty) return;
    if (ui_grow((void **)&s->style_dirty, &s->style_dirty_capacity, s->style_dirty_count + 1,
                sizeof *s->style_dirty) != UI_OK) {
        UI_ERROR(s->log, MODULE, "out of memory queueing a style resolution");
        return;
    }
    n->style_dirty = true;
    s->style_dirty[s->style_dirty_count++] = slot;
}

// Marks a node's layout dirty, and every ancestor's: their sizes may depend on it.
static void layout_dirty(ui_scene *s, uint32_t slot) {
    uint32_t root = s->nodes[slot].root;
    while (slot != UI_NO_SLOT && !s->nodes[slot].layout.dirty) {
        s->nodes[slot].layout.dirty = true;
        slot = s->nodes[slot].parent;
    }
    s->roots[root].layout_dirty = true;
    s->roots[root].dirty = true;
}

// The container a node's layout depends on: its parent, or the surface.
static void parent_layout_dirty(ui_scene *s, uint32_t slot) {
    const ui_node *n = &s->nodes[slot];
    if (n->parent != UI_NO_SLOT) layout_dirty(s, n->parent);
    else s->roots[n->root].layout_dirty = true;
}

static ui_node_anim *anim_of(ui_node *n) {
    if (!n->anim) n->anim = calloc(1, sizeof *n->anim);
    return n->anim;
}

// Sets (or with UI_VALUE_UNSET removes) the node's override of one property. It takes over
// the value's string reference. Adding or removing one moves the others: the node's
// cascade, which points at them, is then gathered again before it is read.
static bool set_override(ui_scene *s, ui_node *n, uint16_t prop, const ui_value *v) {
    for (uint32_t i = 0; i < n->override_count; i++) {
        if (n->overrides[i].prop != prop) continue;
        ui_styles_release_value(&s->styles, &n->overrides[i].value);
        if (v->kind == UI_VALUE_UNSET) {
            n->overrides[i] = n->overrides[--n->override_count];
            n->cascade_dirty = true;
        } else {
            n->overrides[i].value = *v;
        }
        return true;
    }
    if (v->kind == UI_VALUE_UNSET) return true;
    uint32_t cap = n->override_capacity;
    if (ui_grow((void **)&n->overrides, &cap, n->override_count + 1u, sizeof *n->overrides) != UI_OK) {
        ui_styles_release_value(&s->styles, v);
        return false;
    }
    n->override_capacity = (uint16_t)(cap > UINT16_MAX ? UINT16_MAX : cap);
    n->overrides[n->override_count++] = (ui_style_entry){prop, *v};
    n->cascade_dirty = true;
    return true;
}

void ui_scene_set_root_direction(ui_scene *s, ui_handle surface, uint8_t direction) {
    uint32_t i = ui_scene_root_index(s, surface);
    if (i == UI_MAX_SURFACES || s->roots[i].computed.text.direction == direction) return;
    s->roots[i].computed.text.direction = direction;
    for (uint32_t slot = s->roots[i].first_child; slot != UI_NO_SLOT; slot = s->nodes[slot].next_sibling)
        mark_style_dirty(s, slot, false);
    s->roots[i].layout_dirty = true;
}

static bool apply_create(ui_scene *s, const uint8_t *p) {
    ui_handle node = ui_wire_get_handle(p), parent = ui_wire_get_handle(p + 8);
    ui_scene_root *root = NULL;
    ui_node *parent_node = NULL;
    if (ui_handle_kind_of(parent) == UI_HANDLE_SURFACE) {
        root = find_root(s, parent);
        if (!root) return drop(s, "create_node (parent surface)", parent);
    } else {
        parent_node = node_of(s, parent);
        if (!parent_node) return drop(s, "create_node (parent)", parent);
    }
    ui_result r = ui_handle_table_claim(&s->handles, node);
    if (r != UI_OK) return drop(s, "create_node", node);
    r = ui_grow((void **)&s->nodes, &s->node_capacity, ui_handle_table_capacity(&s->handles),
                sizeof *s->nodes);
    if (r != UI_OK) {
        ui_handle_table_release(&s->handles, node);
        UI_ERROR(s->log, MODULE, "out of memory creating a node");
        s->stats.dropped++;
        return false;
    }
    if (parent_node) parent_node = node_of(s, parent); // nodes may have moved
    uint32_t slot = ui_handle_slot(node);
    ui_node *n = &s->nodes[slot];
    memset(n, 0, sizeof *n);
    n->first_child = n->last_child = n->next_sibling = UI_NO_SLOT;
    ui_computed_init(&n->computed);
    n->opacity = 1;
    uint32_t *first, *last;
    if (root) {
        n->parent = UI_NO_SLOT;
        n->root = (uint32_t)(root - s->roots);
        first = &root->first_child;
        last = &root->last_child;
    } else {
        n->parent = ui_handle_slot(parent);
        n->root = parent_node->root;
        first = &parent_node->first_child;
        last = &parent_node->last_child;
    }
    n->prev_sibling = *last;
    if (*last != UI_NO_SLOT) s->nodes[*last].next_sibling = slot;
    else *first = slot;
    *last = slot;
    s->roots[n->root].dirty = true;
    mark_style_dirty(s, slot, true);
    layout_dirty(s, slot);
    return true;
}

static bool apply_node_op(ui_scene *s, const ui_wire_op *op, uint64_t now_ns);

bool ui_scene_font_path_valid(const uint8_t *payload, const char **path, uint32_t *len) {
    *len = ui_wire_get_u16(payload + 4);
    *path = (const char *)payload + ui_wire_load_font_header(payload);
    return *len && *len <= UI_FONT_PATH_MAX && !memchr(*path, 0, *len) &&
           ui_utf8_valid((const uint8_t *)*path, *len);
}

static bool apply_load_font(ui_scene *s, const ui_wire_op *op, uint32_t offset) {
    const char *path;
    uint32_t len;
    if (!ui_scene_font_path_valid(op->payload, &path, &len)) {
        s->stats.dropped++;
        UI_DEBUG_REPORT(s->log, MODULE, "dropped load_font %u: bad path",
                        ui_wire_get_u32(op->payload));
        return false;
    }
    if (s->font_fn) s->font_fn(s->font_user, ui_wire_get_u32(op->payload), path, len, offset);
    return true;
}

// Styles and tokens belong to the scene, not to a node.
static bool apply_style_op(ui_scene *s, const ui_wire_op *op) {
    bool ok;
    const char *what;
    switch (op->opcode) {
    case UI_OP_DEFINE_STYLE:
        ok = ui_styles_define(&s->styles, op->payload, op->payload_size);
        what = "define_style";
        break;
    case UI_OP_DESTROY_STYLE:
        ok = ui_styles_destroy(&s->styles, ui_wire_get_u32(op->payload));
        what = "destroy_style";
        break;
    default:
        ok = ui_styles_set_token(&s->styles, op->payload, op->payload_size);
        what = "set_token";
        break;
    }
    if (!ok) {
        s->stats.dropped++;
        UI_DEBUG_REPORT(s->log, MODULE, "dropped %s %u: invalid, unknown or out of memory", what,
                        ui_wire_get_u32(op->payload));
        (void)what;
    }
    return ok;
}

static bool apply_op(ui_scene *s, const ui_wire_op *op, uint64_t now_ns) {
    const uint8_t *p = op->payload;
    if (op->opcode == UI_OP_CREATE_NODE) return apply_create(s, p);
    if (op->opcode == UI_OP_SET_TIMER) {
        if (s->timer_fn) s->timer_fn(s->timer_user, ui_wire_get_u32(p), ui_wire_get_u64(p + 8));
        return true;
    }
    if (op->opcode == UI_OP_DEFINE_STYLE || op->opcode == UI_OP_DESTROY_STYLE ||
        op->opcode == UI_OP_SET_TOKEN)
        return apply_style_op(s, op);
    // An unknown op comes from a newer writer and may be any size, even shorter than a
    // handle: drop it before reading anything.
    switch (op->opcode) {
    case UI_OP_SET_STYLES:
    case UI_OP_SET_PROPS:
    case UI_OP_SET_STATE:
    case UI_OP_MOVE_NODE:
    case UI_OP_REQUEST_LAYOUT:
    case UI_OP_DESTROY_NODE:
    case UI_OP_SET_RECT:
    case UI_OP_SET_CORNER_RADIUS:
    case UI_OP_SET_COLOR:
    case UI_OP_SET_CLIP:
    case UI_OP_START_SPRING: break;
    default:
        s->stats.dropped++;
        UI_DEBUG_REPORT(s->log, MODULE, "dropped unknown op %u", op->opcode);
        return false;
    }
    // Every other known op addresses one node; a change marks that node's surface dirty. A
    // layout request changes nothing: it never makes a frame.
    ui_handle h = ui_wire_get_handle(p);
    ui_node *n = node_of(s, h);
    uint32_t root = n ? n->root : 0;
    bool applied = apply_node_op(s, op, now_ns);
    if (applied && n && op->opcode != UI_OP_REQUEST_LAYOUT) s->roots[root].dirty = true;
    return applied;
}

// set_rect's frame as overrides: absolutely positioned at (x, y) in the parent's padding
// box, at the frame's size (docs/layout.md). Also written straight into the computed style,
// so a spring moving the frame needs no resolution every frame.
static bool apply_frame(ui_scene *s, ui_node *n) {
    static const uint16_t props[4] = {UI_STYLE_LEFT, UI_STYLE_TOP, UI_STYLE_WIDTH, UI_STYLE_HEIGHT};
    bool ok = set_override(s, n, UI_STYLE_POSITION, &(ui_value){.kind = UI_VALUE_KEYWORD, .keyword = UI_POSITION_ABSOLUTE});
    for (int i = 0; ok && i < 4; i++)
        ok = set_override(s, n, props[i], &(ui_value){.kind = UI_VALUE_PX, .number = n->frame[i]});
    ui_layout_style *l = &n->computed.layout;
    l->position = UI_POSITION_ABSOLUTE;
    l->inset[3] = (ui_length){n->frame[0], UI_LEN_PX};
    l->inset[0] = (ui_length){n->frame[1], UI_LEN_PX};
    l->size[0] = (ui_length){n->frame[2], UI_LEN_PX};
    l->size[1] = (ui_length){n->frame[3], UI_LEN_PX};
    layout_dirty(s, (uint32_t)(n - s->nodes));
    return ok;
}

// A set_* op on a paint property: the override changes, and the value jumps unless the
// styles declare a transition for it.
static bool set_paint(ui_scene *s, ui_node *n, uint16_t prop, ui_value v, int spring) {
    if (!set_override(s, n, prop, &v)) return false;
    if (spring >= 0) n->jump |= (uint16_t)(1u << spring);
    mark_style_dirty(s, (uint32_t)(n - s->nodes), true);
    return true;
}

static bool apply_set_styles(ui_scene *s, ui_node *n, const uint8_t *p) {
    uint32_t count = ui_wire_get_u16(p + 8);
    const uint8_t *ids = p + ui_wire_set_styles_header(p);
    uint32_t *list = NULL;
    if (count && !(list = malloc(count * sizeof *list))) return false;
    for (uint32_t i = 0; i < count; i++) {
        list[i] = ui_wire_get_u32(ids + 4 * i);
        if (!list[i] || i >= UINT8_MAX) {
            free(list);
            return false;
        }
    }
    free(n->styles);
    n->styles = list;
    n->style_count = (uint16_t)count;
    mark_style_dirty(s, (uint32_t)(n - s->nodes), true);
    return true;
}

static bool apply_set_props(ui_scene *s, ui_node *n, const uint8_t *p, uint32_t size) {
    uint32_t header = ui_wire_set_props_header(p), count = ui_wire_get_u16(p + 10), decoded;
    ui_style_entry local[16], *e = count <= 16 ? local : malloc(count * sizeof *e);
    if (!e) return false;
    bool ok = ui_style_decode(&s->styles, p + header, size - header, count, true, e, &decoded);
    uint32_t i = 0;
    for (; ok && i < decoded; i++) ok = set_override(s, n, e[i].prop, &e[i].value);
    for (; i < decoded; i++) ui_styles_release_value(&s->styles, &e[i].value); // not taken over
    if (e != local) free(e);
    mark_style_dirty(s, (uint32_t)(n - s->nodes), true); // even partly applied
    return ok;
}

// Sets the root of every node in a subtree (it moved to another surface).
static void set_root(ui_scene *s, uint32_t slot, uint32_t root) {
    uint32_t top = slot;
    for (uint32_t cur = slot; cur != UI_NO_SLOT;) {
        s->nodes[cur].root = root;
        if (s->nodes[cur].first_child != UI_NO_SLOT) {
            cur = s->nodes[cur].first_child;
            continue;
        }
        while (cur != top && s->nodes[cur].next_sibling == UI_NO_SLOT) cur = s->nodes[cur].parent;
        cur = cur == top ? UI_NO_SLOT : s->nodes[cur].next_sibling;
    }
}

// Where a node's presentation rect puts its border box's origin on the surface (UI_NO_SLOT:
// the surface's origin).
static void presented_origin(const ui_scene *s, uint32_t slot, float out[2]) {
    out[0] = out[1] = 0;
    for (; slot != UI_NO_SLOT; slot = s->nodes[slot].parent) {
        out[0] += s->nodes[slot].rect[0];
        out[1] += s->nodes[slot].rect[1];
    }
}

// A node moved under another parent stays where it is drawn: its presentation rect, and a
// running layout animation, are re-expressed relative to the new parent, and the next
// layout places it (animating from there with a layout transition). Moved to another
// surface, it appears at its new layout rect.
static void rebase(ui_scene *s, ui_node *n, const float from[2], uint32_t old_root) {
    if (n->root != old_root) {
        n->placed = false;
        if (n->anim) n->anim->springs[UI_SPRING_LAYOUT].active = false;
        return;
    }
    float to[2];
    presented_origin(s, n->parent, to);
    ui_spring *sp = n->anim ? &n->anim->springs[UI_SPRING_LAYOUT] : NULL;
    for (int i = 0; i < 2; i++) {
        float d = from[i] - to[i];
        n->rect[i] += d;
        if (sp && sp->active) {
            sp->position[i] += d;
            sp->target[i] += d;
        }
    }
    n->rebased = true;
}

// move_node: under `parent` (a node or a surface), before sibling `before` (null: last).
static bool apply_move(ui_scene *s, ui_node *n, const uint8_t *p) {
    uint32_t slot = (uint32_t)(n - s->nodes);
    ui_handle parent = ui_wire_get_handle(p + 8), before = ui_wire_get_handle(p + 16);
    ui_scene_root *root = NULL;
    ui_node *pn = NULL;
    if (ui_handle_kind_of(parent) == UI_HANDLE_SURFACE) {
        if (!(root = find_root(s, parent))) return false;
    } else {
        if (!(pn = node_of(s, parent))) return false;
        // Not under itself: the new parent can't be the node or one of its descendants.
        for (uint32_t up = (uint32_t)(pn - s->nodes); up != UI_NO_SLOT; up = s->nodes[up].parent)
            if (up == slot) return false;
    }
    uint32_t parent_slot = pn ? (uint32_t)(pn - s->nodes) : UI_NO_SLOT;
    uint32_t root_index = pn ? pn->root : (uint32_t)(root - s->roots);
    uint32_t before_slot = UI_NO_SLOT;
    if (!ui_handle_is_null(before)) {
        ui_node *bn = node_of(s, before);
        if (!bn || bn == n || bn->parent != parent_slot || bn->root != root_index) return false;
        before_slot = (uint32_t)(bn - s->nodes);
    }
    parent_layout_dirty(s, slot);
    s->roots[n->root].dirty = true;
    float from[2];
    presented_origin(s, n->parent, from);
    uint32_t old_root = n->root;
    unlink(s, slot);
    uint32_t *first = pn ? &pn->first_child : &root->first_child;
    uint32_t *last = pn ? &pn->last_child : &root->last_child;
    n->parent = parent_slot;
    n->next_sibling = before_slot;
    n->prev_sibling = before_slot != UI_NO_SLOT ? s->nodes[before_slot].prev_sibling : *last;
    if (n->prev_sibling != UI_NO_SLOT) s->nodes[n->prev_sibling].next_sibling = slot;
    else *first = slot;
    if (before_slot != UI_NO_SLOT) s->nodes[before_slot].prev_sibling = slot;
    else *last = slot;
    if (n->root != root_index) set_root(s, slot, root_index);
    rebase(s, n, from, old_root);
    s->roots[root_index].dirty = true;
    layout_dirty(s, slot);
    mark_style_dirty(s, slot, false); // it inherits from its new parent
    return true;
}

static bool apply_node_op(ui_scene *s, const ui_wire_op *op, uint64_t now_ns) {
    const uint8_t *p = op->payload;
    ui_handle h = ui_wire_get_handle(p);
    ui_node *n = node_of(s, h);
    float v[UI_PROP_MAX_COMPONENTS];
    switch (op->opcode) {
    case UI_OP_DESTROY_NODE:
        if (!n) return drop(s, "destroy_node", h);
        if (!destroy_subtree(s, ui_handle_slot(h))) {
            s->stats.dropped++;
            return false;
        }
        return true;
    case UI_OP_SET_RECT:
        if (!n) return drop(s, "set_rect", h);
        for (int i = 0; i < 4; i++) v[i] = ui_wire_get_f32(p + 8 + 4 * i);
        if (!finite_n(v, 4) || v[2] < 0 || v[3] < 0) return drop(s, "set_rect (value)", h);
        memcpy(n->frame, v, sizeof n->frame);
        n->framed = true;
        if (n->anim) n->anim->springs[UI_SPRING_RECT].active = false;
        if (!apply_frame(s, n)) return drop(s, "set_rect (out of memory)", h);
        mark_style_dirty(s, (uint32_t)(n - s->nodes), true);
        return true;
    case UI_OP_SET_CORNER_RADIUS:
        if (!n) return drop(s, "set_corner_radius", h);
        v[0] = ui_wire_get_f32(p + 8);
        if (!isfinite(v[0]) || v[0] < 0) return drop(s, "set_corner_radius (value)", h);
        if (!set_paint(s, n, UI_STYLE_CORNER_RADIUS, ui_px(v[0]), UI_SPRING_RADIUS))
            return drop(s, "set_corner_radius (out of memory)", h);
        return true;
    case UI_OP_SET_COLOR:
        if (!n) return drop(s, "set_color", h);
        for (int i = 0; i < 4; i++) v[i] = ui_wire_get_f32(p + 8 + 4 * i);
        if (!finite_n(v, 4)) return drop(s, "set_color (value)", h);
        if (!set_paint(s, n, UI_STYLE_BACKGROUND_COLOR, ui_rgba(v[0], v[1], v[2], v[3]), UI_SPRING_COLOR))
            return drop(s, "set_color (out of memory)", h);
        return true;
    case UI_OP_SET_CLIP: {
        if (!n) return drop(s, "set_clip", h);
        uint32_t flags = ui_wire_get_u32(p + 8);
        if (flags & ~1u) return drop(s, "set_clip (flags)", h);
        if (!set_paint(s, n, UI_STYLE_CLIP, ui_keyword(flags & 1u), -1))
            return drop(s, "set_clip (out of memory)", h);
        return true;
    }
    case UI_OP_START_SPRING: {
        if (!n) return drop(s, "start_spring", h);
        ui_property prop = (ui_property)ui_wire_get_u16(p + 8);
        uint32_t count = ui_wire_get_u16(p + 10);
        float stiffness = ui_wire_get_f32(p + 12), damping = ui_wire_get_f32(p + 16);
        if (ui_property_components(prop) != count || !isfinite(stiffness) || stiffness <= 0 ||
            !isfinite(damping) || damping < 0)
            return drop(s, "start_spring (property)", h);
        for (uint32_t i = 0; i < count; i++) v[i] = ui_wire_get_f32(p + 20 + 4 * i);
        if (!finite_n(v, count)) return drop(s, "start_spring (target)", h);
        ui_node_anim *a = anim_of(n);
        if (!a) return drop(s, "start_spring (out of memory)", h);
        if (prop == UI_PROP_RECT) {
            // The spring moves set_rect's frame; a node without one starts from where it is.
            if (!n->framed) {
                memcpy(n->frame, n->rect, sizeof n->frame);
                n->framed = true;
            }
            ui_spring_start(&a->springs[UI_SPRING_RECT], count, n->frame, v, stiffness, damping, now_ns);
            if (!apply_frame(s, n)) return drop(s, "start_spring (out of memory)", h);
            return true;
        }
        // Paint properties: the override becomes the target, and the spring starts toward
        // whatever that resolves to, with these parameters.
        bool ok = prop == UI_PROP_COLOR
                      ? set_override(s, n, UI_STYLE_BACKGROUND_COLOR, &(ui_value){.kind = UI_VALUE_COLOR, .color = {v[0], v[1], v[2], v[3]}})
                      : set_override(s, n, UI_STYLE_CORNER_RADIUS, &(ui_value){.kind = UI_VALUE_PX, .number = v[0]});
        if (!ok) return drop(s, "start_spring (out of memory)", h);
        int i = prop == UI_PROP_COLOR ? UI_SPRING_COLOR : UI_SPRING_RADIUS;
        a->requested[i] = true;
        a->requested_params[i][0] = stiffness;
        a->requested_params[i][1] = damping;
        n->jump &= (uint16_t)~(1u << i);
        mark_style_dirty(s, (uint32_t)(n - s->nodes), true);
        return true;
    }
    case UI_OP_SET_STYLES:
        if (!n) return drop(s, "set_styles", h);
        if (!apply_set_styles(s, n, p)) return drop(s, "set_styles (ids)", h);
        return true;
    case UI_OP_SET_PROPS:
        if (!n) return drop(s, "set_props", h);
        if (!apply_set_props(s, n, p, op->payload_size)) return drop(s, "set_props (values)", h);
        return true;
    case UI_OP_MOVE_NODE:
        if (!n) return drop(s, "move_node", h);
        if (!apply_move(s, n, p)) return drop(s, "move_node (parent or sibling)", h);
        return true;
    case UI_OP_REQUEST_LAYOUT:
        // Queued even for a stale handle: the logic waits for an answer either way.
        if (ui_grow((void **)&s->requests, &s->request_capacity, s->request_count + 1, sizeof *s->requests) !=
            UI_OK)
            return drop(s, "request_layout (out of memory)", h);
        s->requests[s->request_count++] = (ui_scene_request){h, ui_wire_get_u32(p + 8)};
        return true;
    case UI_OP_SET_STATE: {
        if (!n) return drop(s, "set_state", h);
        uint32_t mask = ui_wire_get_u32(p + 8), value = ui_wire_get_u32(p + 12);
        uint8_t state = (uint8_t)((n->state & ~mask) | (value & mask));
        if (state != n->state) mark_style_dirty(s, (uint32_t)(n - s->nodes), true);
        n->state = state;
        return true;
    }
    default:
        s->stats.dropped++;
        UI_DEBUG_REPORT(s->log, MODULE, "dropped unknown op %u", op->opcode);
        return false;
    }
}

// ---- set_text ----------------------------------------------------------------------------

// Applies one set_text: the prepared one for this op when there is one, else it is decoded
// here, consuming the append_text ops that continue it from `r`. A drop leaves the node
// untouched.
static bool apply_set_text(ui_scene *s, const ui_wire_op *op, ui_wire_reader *r,
                           ui_scene_text_prep *prep) {
    ui_text_op local, *t = &local;
    if (prep) {
        t = &prep->op;
        for (uint32_t i = 0; i < t->continuations; i++) {
            ui_wire_op skip;
            ui_wire_reader_next(r, &skip); // decoded from these same bytes: append_text ops
        }
    } else {
        ui_text_op_decode(op, r, &local);
    }
    ui_node *n = node_of(s, t->node);
    const char *why = !n ? "set_text" : t->why;
    if (!why && !n->text && !(n->text = calloc(1, sizeof *n->text))) why = "set_text (out of memory)";
    if (why) {
        ui_handle h = t->node;
        ui_text_op_free(t);
        return drop(s, why, h);
    }
    ui_paragraph_set_source(n->text, t->source);
    ui_text_source_release(n->text_base);
    n->text_base = ui_text_source_retain(t->source);
    n->text_keyed = false; // resolution checks what it shapes with
    // Its family and language stand in for the inherited ones (style.h): kept interned.
    const char *family, *language;
    uint32_t family_len, language_len;
    ui_text_input in = ui_text_source_input(t->source);
    ui_text_input_family(&in, &in.base, &family, &family_len);
    ui_text_input_language(&in, &in.base, &language, &language_len);
    const char *old_family = n->text_family, *old_language = n->text_language;
    n->text_family = family_len ? ui_styles_intern(&s->styles, family, family_len) : NULL;
    n->text_language = language && language_len ? ui_styles_intern(&s->styles, language, language_len) : NULL;
    ui_styles_release(&s->styles, old_family);
    ui_styles_release(&s->styles, old_language);
    // What preparation shaped is adopted once the node's values are known (key_text).
    for (uint32_t i = 0; prep && prep->shaped_source && i < prep->scale_count; i++)
        ui_scene_offer_shaped(s, t->node, t->source, prep->shaped_source, prep->scales[i], &prep->shaped[i]);
    s->stats.text_invalid += t->replaced;
    if (t->replaced)
        UI_DEBUG_REPORT(s->log, MODULE, "set_text: replaced %u ill-formed UTF-8 sequence(s)", t->replaced);
    ui_text_op_free(t);
    s->roots[n->root].dirty = true;
    mark_style_dirty(s, (uint32_t)(n - s->nodes), false); // set_text's values stand in for inherited ones
    layout_dirty(s, (uint32_t)(n - s->nodes));
    return true;
}

ui_result ui_scene_apply(ui_scene *s, const uint8_t *data, size_t size, uint64_t now_ns) {
    return ui_scene_apply_prepared(s, data, size, now_ns, NULL);
}

ui_result ui_scene_apply_prepared(ui_scene *s, const uint8_t *data, size_t size, uint64_t now_ns,
                                  const ui_scene_prepared *prep) {
    ui_result res = ui_scene_validate(data, size);
    if (res != UI_OK) {
        s->stats.rejected++;
        UI_DEBUG_REPORT(s->log, MODULE, "rejected malformed command buffer (%zu bytes)", size);
        return res;
    }
    ui_wire_reader r;
    ui_wire_reader_init(&r, UI_WIRE_MAGIC_CMD, data, size);
    ui_wire_op op;
    uint32_t next_text = 0;
    while (ui_wire_reader_next(&r, &op) == UI_OK) {
        bool applied;
        uint32_t offset = (uint32_t)(op.payload - UI_WIRE_OP_HEADER_SIZE - data);
        if (op.opcode == UI_OP_SET_TEXT) {
            ui_scene_text_prep *tp = NULL;
            if (prep && next_text < prep->text_count && prep->texts[next_text].offset == offset)
                tp = &prep->texts[next_text++];
            if (tp && !tp->decoded) tp = NULL;
            applied = apply_set_text(s, &op, &r, tp);
        } else if (op.opcode == UI_OP_LOAD_FONT) {
            applied = apply_load_font(s, &op, offset);
        } else if (op.opcode == UI_OP_APPEND_TEXT) {
            applied = drop(s, "append_text (no set_text before it)", ui_wire_get_handle(op.payload));
        } else {
            applied = apply_op(s, &op, now_ns);
        }
        if (applied) s->stats.applied++;
    }
    return UI_OK;
}

// ---- Styles and animation ----------------------------------------------------------------

// The value a spring moves (what is drawn).
static float *presented(ui_node *n, int spring) {
    switch (spring) {
    case UI_SPRING_RECT: return n->frame;
    case UI_SPRING_LAYOUT: return n->rect;
    case UI_SPRING_COLOR: return n->color;
    case UI_SPRING_BORDER_TOP:
    case UI_SPRING_BORDER_RIGHT:
    case UI_SPRING_BORDER_BOTTOM:
    case UI_SPRING_BORDER_LEFT: return n->border_color[spring - UI_SPRING_BORDER_TOP];
    case UI_SPRING_RADIUS: return n->radius;
    case UI_SPRING_OPACITY: return &n->opacity;
    default: return n->text_color;
    }
}

static void write_back(ui_scene *s, ui_node *n, int which) {
    ui_spring *sp = &n->anim->springs[which];
    float *dst = presented(n, which);
    for (uint32_t i = 0; i < sp->components; i++) dst[i] = (float)sp->position[i];
    // Springs may overshoot; clamp what is shown, not the spring state.
    if (which == UI_SPRING_RECT) {
        n->frame[2] = fmaxf(n->frame[2], 0.f);
        n->frame[3] = fmaxf(n->frame[3], 0.f);
        if (!apply_frame(s, n)) UI_ERROR(s->log, MODULE, "out of memory moving a frame");
    } else if (which == UI_SPRING_LAYOUT) {
        n->rect[2] = fmaxf(n->rect[2], 0.f);
        n->rect[3] = fmaxf(n->rect[3], 0.f);
    } else if (which == UI_SPRING_RADIUS) {
        for (int i = 0; i < 4; i++) n->radius[i] = fmaxf(n->radius[i], 0.f);
    } else {
        for (uint32_t i = 0; i < sp->components; i++) dst[i] = fminf(fmaxf(dst[i], 0.f), 1.f);
    }
}

// Moves one paint property toward its resolved `target`. A spring start_spring asked for
// starts or retargets with its own parameters. Otherwise, when the target changed or a
// set_* op touched the property, the declared transition starts or retargets (keeping a
// running spring's velocity); without one, or on a node's first resolution, the value jumps
// and any spring stops, as set_* always did. Returns true if anything changed.
static bool update_paint(ui_node *n, int i, const float *target, const float transition[2],
                         uint64_t now_ns) {
    uint32_t comps = spring_components[i];
    float *cur = presented(n, i);
    ui_node_anim *a = n->anim;
    if (a && a->requested[i]) {
        a->requested[i] = false;
        ui_spring_start(&a->springs[i], comps, cur, target, a->requested_params[i][0],
                        a->requested_params[i][1], now_ns);
        return true;
    }
    bool running = a && a->springs[i].active;
    bool changed = false;
    for (uint32_t k = 0; k < comps; k++)
        changed |= running ? a->springs[i].target[k] != (double)target[k] : cur[k] != target[k];
    bool touched = n->jump & (1u << i);
    if (!changed && !touched) return false;
    if (n->resolved && transition[0] > 0 && (a = anim_of(n))) {
        ui_spring_start(&a->springs[i], comps, cur, target, transition[0], transition[1], now_ns);
        return true;
    }
    if (a) a->springs[i].active = false;
    memcpy(cur, target, comps * sizeof *cur);
    return true;
}

// set_text's values: they replace the inherited ones (and max_lines' initial value), so
// the node's styles still override them (docs/styles.md, "Inheritance").
static void text_start(const ui_node *n, ui_computed *c) {
    const ui_text_source *src = n->text_base;
    if (!src) return;
    uint32_t set = ~(uint32_t)src->from_node; // what set_text sets itself
    if (set & UI_TEXT_SET_COLOR) memcpy(c->text.color, src->base.color, sizeof c->text.color);
    if (set & UI_TEXT_SET_FAMILY) c->text.family = n->text_family;
    if (set & UI_TEXT_SET_LANGUAGE) c->text.language = n->text_language;
    if (set & UI_TEXT_SET_SIZE) c->text.size = src->base.size;
    if (set & UI_TEXT_SET_WEIGHT) c->text.weight = src->base.weight;
    if (set & UI_TEXT_SET_SLANT) c->text.slant = src->base.style;
    if (set & UI_TEXT_SET_LINE_HEIGHT) c->text.line_height = src->line_height;
    if (set & UI_TEXT_SET_ALIGN) c->text.align = (uint8_t)src->align;
    c->max_lines = src->max_lines;
}

// ---- Text values and shaping keys ----------------------------------------------------------

static const uint32_t TEXT_FIELDS[] = {UI_TEXT_SET_FAMILY, UI_TEXT_SET_SIZE, UI_TEXT_SET_WEIGHT,
                                       UI_TEXT_SET_SLANT, UI_TEXT_SET_LANGUAGE, UI_TEXT_SET_COLOR,
                                       UI_TEXT_SET_LINE_HEIGHT, UI_TEXT_SET_ALIGN, UI_TEXT_SET_DIRECTION};

// Copies one field of text values.
static void copy_field(ui_text_values *dst, const ui_text_values *src, uint32_t field) {
    switch (field) {
    case UI_TEXT_SET_FAMILY: dst->family = src->family; break;
    case UI_TEXT_SET_SIZE: dst->size = src->size; break;
    case UI_TEXT_SET_WEIGHT: dst->weight = src->weight; break;
    case UI_TEXT_SET_SLANT: dst->slant = src->slant; break;
    case UI_TEXT_SET_LANGUAGE: dst->language = src->language; break;
    case UI_TEXT_SET_COLOR: memcpy(dst->color, src->color, sizeof dst->color); break;
    case UI_TEXT_SET_LINE_HEIGHT: dst->line_height = src->line_height; break;
    case UI_TEXT_SET_ALIGN: dst->align = src->align; break;
    default: dst->direction = src->direction; break;
    }
}

bool ui_text_values_eq(const ui_text_values *a, const ui_text_values *b) {
    return !memcmp(a->color, b->color, sizeof a->color) && ui_istr_eq(a->family, b->family) &&
           ui_istr_eq(a->language, b->language) && a->size == b->size && a->line_height == b->line_height &&
           a->weight == b->weight && a->slant == b->slant && a->align == b->align && a->direction == b->direction;
}

// The fields `base` sets itself and the styles leave alone.
static uint32_t explicit_fields(const ui_text_ctx *c, const ui_text_source *base) {
    return base ? ~(uint32_t)base->from_node & ~c->declared & UI_TEXT_SET_ALL : 0;
}

ui_text_resolved ui_text_ctx_key(const ui_text_ctx *c, const ui_text_source *base) {
    const ui_text_values *v = &c->values;
    uint32_t own = explicit_fields(c, base);
    ui_text_resolved r = {
        .family = v->family, .family_len = ui_istr_len(v->family),
        .language = v->language, .language_len = ui_istr_len(v->language),
        .size = own & UI_TEXT_SET_SIZE ? base->base.size : v->size,
        .weight = own & UI_TEXT_SET_WEIGHT ? base->base.weight : v->weight,
        .slant = own & UI_TEXT_SET_SLANT ? base->base.style : v->slant,
        .dir = v->direction == UI_DIRECTION_RTL ? UI_BIDI_RTL : UI_BIDI_LTR,
    };
    // A set_text's direction (auto too) belongs to its paragraph, not to the node, so the
    // node's styles don't override it.
    if (base && !(base->from_node & UI_TEXT_SET_DIRECTION)) r.dir = base->dir;
    if (own & (UI_TEXT_SET_FAMILY | UI_TEXT_SET_LANGUAGE)) {
        ui_text_input in = ui_text_source_input(base);
        const char *str;
        uint32_t len;
        if (own & UI_TEXT_SET_FAMILY) {
            ui_text_input_family(&in, &in.base, &str, &len);
            r.family = len ? str : NULL;
            r.family_len = len;
        }
        if (own & UI_TEXT_SET_LANGUAGE) {
            ui_text_input_language(&in, &in.base, &str, &len);
            r.language = str && len ? str : NULL;
            r.language_len = r.language ? len : 0;
        }
    }
    return r;
}

bool ui_text_ctx_values(const ui_text_ctx *c, const ui_text_source *base, ui_styles *strings,
                        ui_text_values *out) {
    *out = c->values;
    uint32_t own = explicit_fields(c, base) & ~(uint32_t)UI_TEXT_SET_DIRECTION;
    if (!own) return true;
    ui_text_resolved r = ui_text_ctx_key(c, base);
    if (own & UI_TEXT_SET_COLOR) memcpy(out->color, base->base.color, sizeof out->color);
    if (own & UI_TEXT_SET_SIZE) out->size = r.size;
    if (own & UI_TEXT_SET_WEIGHT) out->weight = r.weight;
    if (own & UI_TEXT_SET_SLANT) out->slant = r.slant;
    if (own & UI_TEXT_SET_LINE_HEIGHT) out->line_height = base->line_height;
    if (own & UI_TEXT_SET_ALIGN) out->align = (uint8_t)base->align;
    bool ok = true;
    // Interned in the caller's table, which keeps them until it collects (the reference is
    // dropped at once: the caller holds no reference to values).
    if (own & UI_TEXT_SET_FAMILY) {
        out->family = r.family ? ui_styles_intern(strings, r.family, r.family_len) : NULL;
        ok &= !r.family || out->family;
        ui_styles_release(strings, out->family);
    }
    if (own & UI_TEXT_SET_LANGUAGE) {
        out->language = r.language ? ui_styles_intern(strings, r.language, r.language_len) : NULL;
        ok &= !r.language || out->language;
        ui_styles_release(strings, out->language);
    }
    return ok;
}

ui_text_ctx ui_scene_text_ctx(const ui_scene *s, uint32_t slot) {
    const ui_node *n = &s->nodes[slot];
    const ui_computed *parent = n->parent != UI_NO_SLOT ? &s->nodes[n->parent].computed : &s->roots[n->root].computed;
    ui_text_ctx c = {parent->text, n->text_declared};
    for (size_t i = 0; i < UI_ARRAY_LEN(TEXT_FIELDS); i++) {
        if (c.declared & TEXT_FIELDS[i]) copy_field(&c.values, &n->computed.text, TEXT_FIELDS[i]);
    }
    return c;
}

void ui_scene_offer_shaped(ui_scene *s, ui_handle node, ui_text_source *base, ui_text_source *source,
                           float scale, ui_shaped_paragraph *shaped) {
    ui_node *n = node_of(s, node);
    if (!n || scale != s->roots[n->root].scale ||
        ui_grow((void **)&s->offers, &s->offer_capacity, s->offer_count + 1, sizeof *s->offers) != UI_OK) {
        ui_shaped_paragraph_free(shaped); // shaped on the core instead, if needed at all
        return;
    }
    ui_scene_offer *o = &s->offers[s->offer_count++];
    *o = (ui_scene_offer){node, ui_text_source_retain(base), ui_text_source_retain(source), scale, *shaped};
    memset(shaped, 0, sizeof *shaped);
    n->offer = s->offer_count;
    mark_style_dirty(s, (uint32_t)(n - s->nodes), false);
}

static void drop_offers(ui_scene *s) {
    for (uint32_t i = 0; i < s->offer_count; i++) {
        ui_node *n = node_of(s, s->offers[i].node);
        if (n) n->offer = 0;
        ui_text_source_release(s->offers[i].base);
        ui_text_source_release(s->offers[i].source);
        ui_shaped_paragraph_free(&s->offers[i].shaped);
    }
    s->offer_count = 0;
}

// The paragraph shows a source that comes from the node's set_text (it, or one derived from it).
static bool from_base(const ui_node *n) {
    const ui_text_source *src = n->text->source;
    return src == n->text_base || (src && src->shared == n->text_base);
}

// Points the paragraph at what it must shape now: prepared shaping when some was offered for
// it, else set_text's source when that already shapes with the node's values, else a source
// derived from it (shaped on the core when laid out). Returns true if the paragraph's
// source changed.
static bool key_text(ui_scene *s, ui_node *n) {
    if (!n->text || !n->text_base) return false;
    uint32_t slot = (uint32_t)(n - s->nodes);
    ui_text_ctx ctx = ui_scene_text_ctx(s, slot);
    ui_text_resolved key = ui_text_ctx_key(&ctx, n->text_base);
    n->text_keyed = true;
    if (n->offer) {
        ui_scene_offer *o = &s->offers[n->offer - 1];
        n->offer = 0;
        if (o->base == n->text_base && o->scale == s->roots[n->root].scale &&
            ui_text_source_shapes_as(o->source, &key)) {
            bool changed = n->text->source != o->source;
            if (changed) ui_paragraph_set_source(n->text, o->source);
            ui_paragraph_adopt_shaped(n->text, &o->shaped);
            return changed;
        }
    }
    if (from_base(n) && ui_text_source_shapes_as(n->text->source, &key)) return false;
    ui_text_source *src = n->text_base;
    bool derive = !ui_text_source_shapes_as(src, &key);
    if (derive && ui_text_source_derive(n->text_base, &key, &src) != UI_OK) {
        UI_ERROR(s->log, MODULE, "out of memory styling a paragraph");
        return false; // set_text's own values meanwhile
    }
    bool changed = n->text->source != src;
    if (changed) ui_paragraph_set_source(n->text, src);
    if (derive) ui_text_source_release(src);
    return changed;
}

static const ui_computed *parent_values(const ui_scene *s, const ui_node *n) {
    return n->parent != UI_NO_SLOT ? &s->nodes[n->parent].computed : &s->roots[n->root].computed;
}

// Computes a node's values from its cascade and its parent's, and makes what is drawn and
// laid out follow. Returns what its children must react to: 2 if its inherited values
// changed, 1 if only others did, 0 if nothing did.
static int resolve_node(ui_scene *s, ui_node *n, uint64_t now_ns) {
    if (n->cascade_dirty) {
        ui_style_view view = ui_style_view_of(&s->styles);
        if (!ui_style_cascade(&view, n->styles, n->style_count, n->state, n->overrides,
                              n->override_count, &n->cascade, &n->cascade_count, &n->cascade_capacity)) {
            UI_ERROR(s->log, MODULE, "out of memory resolving styles");
            n->cascade_count = 0; // initial and inherited values only
        }
        n->cascade_dirty = false;
    }
    s->stats.styles_computed++;
    const ui_computed *parent = parent_values(s, n);
    ui_computed old = n->computed, *c = &n->computed;
    ui_computed_start(c, parent);
    text_start(n, c);
    ui_style_view view = ui_style_view_of(&s->styles);
    uint32_t declared;
    n->inherits_other = ui_style_compute(&view, n->cascade, n->cascade_count, parent, c, &declared) &
                        UI_STYLE_INHERITS_OTHER;
    n->text_declared = (uint16_t)declared;
    // What layout reads: the layout values, the border, and the direction (an rtl container
    // mirrors its children).
    if (memcmp(&old.layout, &c->layout, sizeof old.layout) ||
        memcmp(old.border_width, c->border_width, sizeof old.border_width) ||
        old.text.direction != c->text.direction)
        layout_dirty(s, (uint32_t)(n - s->nodes));
    bool changed = false;
    changed |= update_paint(n, UI_SPRING_COLOR, c->background, c->transition[UI_ANIM_BACKGROUND], now_ns);
    for (int side = 0; side < 4; side++)
        changed |= update_paint(n, UI_SPRING_BORDER_TOP + side, c->border_color[side],
                                c->transition[UI_ANIM_BORDER_COLOR], now_ns);
    changed |= update_paint(n, UI_SPRING_RADIUS, c->corner_radius, c->transition[UI_ANIM_RADIUS], now_ns);
    changed |= update_paint(n, UI_SPRING_OPACITY, &c->opacity, c->transition[UI_ANIM_OPACITY], now_ns);
    changed |= update_paint(n, UI_SPRING_TEXT_COLOR, c->text.color, c->transition[UI_ANIM_TEXT_COLOR], now_ns);
    // A layout animation whose transition went away ends at the layout rect.
    if (!(c->transition[UI_ANIM_LAYOUT][0] > 0) && n->anim && n->anim->springs[UI_SPRING_LAYOUT].active) {
        n->anim->springs[UI_SPRING_LAYOUT].active = false;
        memcpy(n->rect, n->layout.rect, sizeof n->rect);
        changed = true;
    }
    changed |= n->clip != c->clip || memcmp(n->border_width, c->border_width, sizeof n->border_width);
    n->clip = c->clip;
    memcpy(n->border_width, c->border_width, sizeof n->border_width);
    if (key_text(s, n)) {
        changed = true;
        layout_dirty(s, (uint32_t)(n - s->nodes)); // shaped differently, so measured differently
    }
    if (n->text) {
        bool valid = n->text->layout_valid;
        ui_paragraph_set_params(n->text, c->max_lines, (ui_align)c->text.align, c->text.line_height);
        if (valid != n->text->layout_valid) {
            changed = true;
            layout_dirty(s, (uint32_t)(n - s->nodes)); // line height and max lines change heights
        }
    }
    n->jump = 0;
    n->resolved = true;
    if (changed) s->roots[n->root].dirty = true;
    if (memcmp(&old.text, &c->text, sizeof old.text)) return 2;
    return memcmp(&old, c, sizeof old) ? 1 : 0;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

// Computes `slot`, then, depth first, every descendant reached through a change it inherits.
// Uses s->scratch as the stack.
static void resolve_subtree(ui_scene *s, uint32_t top, uint64_t now_ns) {
    uint32_t count = 0;
    if (ui_grow((void **)&s->scratch, &s->scratch_capacity, s->handles.live + 1, sizeof *s->scratch) != UI_OK) {
        UI_ERROR(s->log, MODULE, "out of memory resolving styles");
        s->nodes[top].style_dirty = false;
        resolve_node(s, &s->nodes[top], now_ns);
        return;
    }
    s->scratch[count++] = top;
    while (count) {
        uint32_t slot = s->scratch[--count];
        ui_node *n = &s->nodes[slot];
        n->style_dirty = false;
        int change = resolve_node(s, n, now_ns);
        // A child queued itself but not reached here gets its own turn later (it is deeper).
        for (uint32_t c = n->first_child; change && c != UI_NO_SLOT; c = s->nodes[c].next_sibling) {
            if (change == 2 || s->nodes[c].inherits_other) s->scratch[count++] = c;
        }
    }
}

static bool contains_id(const uint32_t *list, uint32_t n, uint32_t id) {
    for (uint32_t i = 0; i < n; i++) {
        if (list[i] == id) return true;
    }
    return false;
}

void ui_scene_resolve(ui_scene *s, uint64_t now_ns) {
    // Styles that were (re)defined or destroyed and tokens that changed: every node using
    // them resolves again.
    uint32_t *styles, style_count, *tokens, token_count;
    if (ui_styles_take_changes(&s->styles, &styles, &style_count, &tokens, &token_count)) {
        for (uint32_t slot = 0; slot < s->handles.used; slot++) {
            ui_node *n = &s->nodes[slot];
            if (!s->handles.alive[slot] || n->style_dirty) continue;
            bool uses = false;
            for (uint32_t k = 0; k < n->style_count && !uses; k++)
                uses = contains_id(styles, style_count, n->styles[k]);
            for (uint32_t k = 0; k < n->override_count && !uses; k++)
                uses = n->overrides[k].value.kind == UI_VALUE_TOKEN &&
                       contains_id(tokens, token_count, n->overrides[k].value.token);
            if (uses) mark_style_dirty(s, slot, true); // a definition may have moved its entries
        }
    }
    free(styles);
    free(tokens);
    // Parents first, so every node computes from its parent's new values: the queue sorted
    // by depth. Each node's children are computed again only when what they inherit changed,
    // so a change stops where descendants declare their own values.
    uint32_t live = 0;
    for (uint32_t i = 0; i < s->style_dirty_count; i++) {
        uint32_t slot = s->style_dirty[i];
        if (slot < s->handles.used && s->handles.alive[slot] && s->nodes[slot].style_dirty)
            s->style_dirty[live++] = slot;
    }
    s->style_dirty_count = live;
    uint64_t *order = live ? malloc(live * sizeof *order) : NULL;
    for (uint32_t i = 0; order && i < live; i++) {
        uint32_t depth = 0;
        for (uint32_t up = s->nodes[s->style_dirty[i]].parent; up != UI_NO_SLOT; up = s->nodes[up].parent) depth++;
        order[i] = (uint64_t)depth << 32 | s->style_dirty[i];
    }
    if (order) qsort(order, live, sizeof *order, cmp_u64);
    else if (live) UI_ERROR(s->log, MODULE, "out of memory ordering style resolution");
    for (uint32_t i = 0; i < live; i++) {
        uint32_t slot = order ? (uint32_t)order[i] : s->style_dirty[i];
        if (!s->nodes[slot].style_dirty) continue; // computed already, under a dirty ancestor
        resolve_subtree(s, slot, now_ns);
    }
    free(order);
    s->style_dirty_count = 0;
    drop_offers(s); // prepared shaping nothing took (its node changed again, or went away)
    ui_styles_collect(&s->styles); // no computed value points at an unreferenced string now
}

void ui_scene_animate(ui_scene *s, uint64_t now_ns) {
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) s->roots[i].animating = false;
    for (uint32_t slot = 0; slot < s->handles.used; slot++) {
        ui_node *n = &s->nodes[slot];
        if (!s->handles.alive[slot] || !n->anim) continue;
        for (int i = 0; i < UI_SPRING_COUNT; i++) {
            ui_spring *sp = &n->anim->springs[i];
            if (!sp->active) continue;
            ui_spring_advance(sp, now_ns, spring_epsilon[i]);
            write_back(s, n, i);
            s->roots[n->root].dirty = true;
            if (sp->active) s->roots[n->root].animating = true;
        }
    }
}

ui_scene_root *ui_scene_root_of(ui_scene *s, ui_handle surface) { return find_root(s, surface); }

bool ui_scene_measure(const ui_scene *s, ui_handle h, float rects[4][4], ui_handle *surface) {
    memset(rects, 0, 16 * sizeof(float));
    *surface = (ui_handle){0};
    uint32_t slot = ui_handle_slot(h);
    if (!ui_handle_table_valid(&s->handles, h)) return false;
    const ui_node *n = &s->nodes[slot];
    *surface = s->roots[n->root].surface;
    for (uint32_t up = slot; up != UI_NO_SLOT; up = s->nodes[up].parent)
        if (s->nodes[up].computed.layout.display == UI_DISPLAY_NONE) return true;
    memcpy(rects[0], n->layout.rect, sizeof rects[0]);
    memcpy(rects[2], n->rect, sizeof rects[2]);
    memcpy(rects[1], rects[0], sizeof rects[1]);
    memcpy(rects[3], rects[2], sizeof rects[3]);
    for (uint32_t up = n->parent; up != UI_NO_SLOT; up = s->nodes[up].parent) {
        rects[1][0] += s->nodes[up].layout.rect[0];
        rects[1][1] += s->nodes[up].layout.rect[1];
        rects[3][0] += s->nodes[up].rect[0];
        rects[3][1] += s->nodes[up].rect[1];
    }
    return true;
}

ui_node *ui_scene_node(ui_scene *s, ui_handle h) { return node_of(s, h); }

ui_handle ui_scene_handle_of(const ui_scene *s, uint32_t slot) { return handle_of(s, slot); }

void ui_scene_fonts_changed(ui_scene *s) {
    for (uint32_t slot = 0; slot < s->handles.used; slot++) {
        if (!s->handles.alive[slot] || !s->nodes[slot].text) continue;
        s->roots[s->nodes[slot].root].dirty = true;
        layout_dirty(s, slot); // text measures differently
    }
}

bool ui_scene_layout(ui_scene *s, ui_handle surface, float width, float height, float scale,
                     ui_text_context *text, uint64_t now_ns) {
    ui_scene_root *root = find_root(s, surface);
    if (!root) return false;
    uint32_t index = (uint32_t)(root - s->roots);
    if (root->layout_scale != scale) {
        // Text measures differently at another scale, and borders snap to other widths.
        for (uint32_t slot = 0; slot < s->handles.used; slot++) {
            const ui_node *n = &s->nodes[slot];
            if (!s->handles.alive[slot] || n->root != index) continue;
            const float *bw = n->computed.border_width;
            if (n->text || bw[0] > 0 || bw[1] > 0 || bw[2] > 0 || bw[3] > 0) layout_dirty(s, slot);
        }
    }
    if (!root->layout_dirty && root->layout_size[0] == width && root->layout_size[1] == height &&
        root->layout_scale == scale)
        return false;
    root->layout_dirty = false;
    root->layout_size[0] = width;
    root->layout_size[1] = height;
    root->layout_scale = scale;
    // No redraw is asked for here: layout runs as a surface draws, and whatever changed
    // what layout depends on already marked the root dirty.
    ui_layout_root(s, index, width, height, scale, text, &s->layout_stats, now_ns);
    return true;
}

void ui_scene_place(ui_scene *s, uint32_t slot, uint64_t now_ns) {
    ui_node *n = &s->nodes[slot];
    const float *target = n->layout.rect, *transition = n->computed.transition[UI_ANIM_LAYOUT];
    bool shown = n->computed.layout.display != UI_DISPLAY_NONE;
    ui_node_anim *a = n->anim;
    bool running = a && a->springs[UI_SPRING_LAYOUT].active;
    // New nodes, and nodes shown again, appear at their layout rect; the others animate
    // from where they are drawn when they have a layout transition. A running animation
    // retargets, keeping its velocity.
    if (n->placed && shown && transition[0] > 0 && (running || memcmp(n->rect, target, sizeof n->rect)) &&
        (a = anim_of(n))) {
        ui_spring_start(&a->springs[UI_SPRING_LAYOUT], 4, n->rect, target, transition[0], transition[1],
                        now_ns);
        // The core reads this after the frame to keep frames coming; the next animate
        // advances the spring.
        s->roots[n->root].animating = true;
    } else {
        if (running) a->springs[UI_SPRING_LAYOUT].active = false;
        memcpy(n->rect, target, sizeof n->rect);
    }
    n->placed = shown;
    n->rebased = false;
}

// Appends the glyphs of a node's text. ox, oy: the node's origin in physical px.
// `base_color` (linear, straight alpha) is the default style's color as resolved for the
// node; spans keep their own.
static ui_result build_text(ui_paragraph *p, const float base_color[4], float ox, float oy,
                            float width, float scale, const float clip[4], ui_display_list *dl,
                            const ui_scene_text *t) {
    ui_result res;
    const ui_paragraph_layout *l = ui_paragraph_layout_get(p, t->ctx, scale, width, &res);
    if (!l) return res == UI_ERR_WRONG_STATE ? UI_OK : res; // no fonts: nothing to draw
    for (uint32_t r = 0; r < l->run_count; r++) {
        const ui_layout_run *run = &l->runs[r];
        ui_font *font = run->sized_font;
        if (!font) font = ui_font_registry_font_by_key(t->ctx->fonts, run->font);
        if (!font) return UI_ERR_OUT_OF_MEMORY;
        for (uint32_t i = 0; i < run->glyph_count; i++) {
            const ui_layout_glyph *g = &l->glyphs[run->glyph_start + i];
            if (g->glyph == UI_GLYPH_INVISIBLE) continue;
            // Horizontal subpixel bins; baselines snap to whole pixels.
            float gx = ox + g->x;
            float px = floorf(gx);
            uint32_t bin = (uint32_t)((gx - px) * UI_ATLAS_SUBPIXEL_BINS + 0.5f);
            if (bin == UI_ATLAS_SUBPIXEL_BINS) {
                bin = 0;
                px += 1;
            }
            float py = roundf(oy + g->y);
            // Cull by line: a glyph is never further than a few em from its pen position.
            float reach = (float)run->font.size_q / 64.f * 4;
            if (px + reach < clip[0] || px - reach > clip[2] || py + reach < clip[1] ||
                py - reach > clip[3])
                continue;
            ui_glyph_key key = {run->font, g->glyph, bin};
            const ui_atlas_glyph *ag = ui_atlas_get(t->atlas, t->raster, font, key);
            if (!ag) return UI_ERR_OUT_OF_MEMORY;
            if (!ag->width) continue;
            float gx0 = px + ag->left, gy0 = py + ag->top;
            if (gx0 >= clip[2] || gy0 >= clip[3] || gx0 + ag->width <= clip[0] ||
                gy0 + ag->height <= clip[1])
                continue; // entirely outside the clip
            const ui_text_span *sp = g->style ? &p->spans[g->style - 1] : NULL;
            const float *c = sp && !(sp->from_node & UI_TEXT_SET_COLOR) ? sp->style.color : base_color;
            float a = c[3];
            ui_dl_glyph dg = {
                .x = px + ag->left, .y = py + ag->top,
                .width = ag->width, .height = ag->height,
                .u = ag->x, .v = ag->y,
                .color = {c[0] * a, c[1] * a, c[2] * a, a},
            };
            res = ui_display_list_push_glyph(dl, ag->page, &dg);
            if (res != UI_OK) return res;
        }
    }
    return UI_OK;
}

// Pixel coordinates for a scissor: [0, 2^30], NaN to 0 (fmaxf returns the other operand).
static float scissor_px(float v) { return fminf(fmaxf(v, 0.f), 0x1p30f); }

// Clips hold whatever the rects produce, huge or infinite (a node may sit at x = 1e30), so
// they are clamped before the conversion to integers. The renderer clamps to the surface.
static ui_dl_clip scissor_of(const float c[4]) {
    if (c[0] == -INFINITY && c[1] == -INFINITY && c[2] == INFINITY && c[3] == INFINITY)
        return UI_DL_NO_CLIP;
    float x0 = floorf(scissor_px(c[0])), y0 = floorf(scissor_px(c[1]));
    float x1 = ceilf(scissor_px(c[2])), y1 = ceilf(scissor_px(c[3]));
    return (ui_dl_clip){(int32_t)x0, (int32_t)y0, (int32_t)fmaxf(x1 - x0, 0),
                        (int32_t)fmaxf(y1 - y0, 0)};
}

static void intersect(float out[4], const float c[4], float x0, float y0, float x1, float y1) {
    out[0] = fmaxf(c[0], x0);
    out[1] = fmaxf(c[1], y0);
    out[2] = fminf(c[2], x1);
    out[3] = fminf(c[3], y1);
}

// A node moves while its frame or its presentation rect springs; then it, and everything
// under it, draws where it is instead of snapping to whole pixels.
static bool moving(const ui_node *n) {
    return n->anim && (n->anim->springs[UI_SPRING_RECT].active || n->anim->springs[UI_SPRING_LAYOUT].active);
}

// A border at rest is whole physical pixels, at least one, as browsers draw them.
static float border_px(float width, float scale, bool snap) {
    if (!(width > 0)) return 0;
    return snap ? fmaxf(floorf(width * scale + 1e-3f), 1.f) : width * scale;
}

ui_result ui_scene_build(ui_scene *s, ui_handle surface, ui_display_list *dl,
                         const ui_scene_text *text) {
    ui_scene_root *root = find_root(s, surface);
    if (!root) return UI_ERR_STALE_HANDLE;
    float scale = (float)dl->scale_120 / 120.f;
    // Iterative pre-order walk. levels[depth] describes the node whose children are being
    // visited: their origin, their clip, whether they move, and whether a layer must close
    // after them.
    if (ui_grow((void **)&s->levels, &s->level_capacity, 1, sizeof *s->levels) != UI_OK)
        return UI_ERR_OUT_OF_MEMORY;
    s->levels[0] = (ui_build_level){UI_NO_SLOT, 0, 0, {-INFINITY, -INFINITY, INFINITY, INFINITY},
                                    false, false};
    uint32_t depth = 0, slot = root->first_child;
    ui_result res;
    for (;;) {
        if (slot == UI_NO_SLOT) {
            if (!depth) break;
            const ui_build_level *done = &s->levels[depth--];
            if (done->layer && (res = ui_display_list_end_layer(dl)) != UI_OK) return res;
            slot = s->nodes[done->slot].next_sibling;
            continue;
        }
        ui_node *n = &s->nodes[slot];
        const ui_build_level *up = &s->levels[depth];
        if (!(n->opacity > 0) || n->computed.layout.display == UI_DISPLAY_NONE) {
            slot = n->next_sibling; // invisible, with its subtree
            continue;
        }
        float x = up->x + n->rect[0], y = up->y + n->rect[1];
        bool move = up->moving || moving(n);
        // At rest, origin and size round separately, so a box keeps its size wherever it
        // sits; while moving, boxes draw at their exact positions.
        float px0 = x * scale, py0 = y * scale, pw = n->rect[2] * scale, ph = n->rect[3] * scale;
        if (!move) {
            px0 = roundf(px0);
            py0 = roundf(py0);
            pw = roundf(pw);
            ph = roundf(ph);
        }
        float px1 = px0 + pw, py1 = py0 + ph;
        float clip[4];
        memcpy(clip, up->clip, sizeof clip);
        dl->clip = scissor_of(clip);
        // Borders per side, in whole device pixels at rest; radii scaled down together so
        // adjacent ones fit each side (CSS backgrounds 3, "Overlapping curves").
        float bw[4], rad[4];
        bool border = false;
        for (int i = 0; i < 4; i++) {
            bw[i] = border_px(n->border_width[i], scale, !move);
            border |= bw[i] > 0 && n->border_color[i][3] > 0;
            rad[i] = n->radius[i] * scale;
        }
        float fit = 1;
        const float sums[4] = {rad[0] + rad[1], rad[3] + rad[2], rad[0] + rad[3], rad[1] + rad[2]};
        const float sides[4] = {pw, pw, ph, ph};
        for (int i = 0; i < 4; i++) {
            if (sums[i] > sides[i] && sums[i] > 0) fit = fminf(fit, sides[i] / sums[i]);
        }
        for (int i = 0; i < 4; i++) rad[i] *= fit;
        bool has_children = n->first_child != UI_NO_SLOT;
        // CSS group opacity: the node and its subtree draw into a layer composited at its
        // opacity. A lone background needs no layer: its alpha is multiplied.
        bool group = n->opacity < 1, lone = !n->text && !has_children && !border;
        bool layer = group && !lone;
        float alpha = group && lone ? n->opacity : 1;
        if (layer && (res = ui_display_list_begin_layer(dl, n->opacity)) != UI_OK) return res;
        bool visible = px1 + 1 > clip[0] && px0 - 1 < clip[2] && py1 + 1 > clip[1] &&
                       py0 - 1 < clip[3]; // 1 px: the rect shader's anti-aliasing margin
        if (pw > 0 && ph > 0 && (n->color[3] > 0 || border) && visible) {
            float a = n->color[3] * alpha;
            const float color[4] = {n->color[0] * a, n->color[1] * a, n->color[2] * a, a};
            float side_color[4][4] = {{0}};
            for (int i = 0; i < 4 && border; i++) {
                float ba = bw[i] > 0 ? n->border_color[i][3] * alpha : 0;
                for (int k = 0; k < 3; k++) side_color[i][k] = n->border_color[i][k] * ba;
                side_color[i][3] = ba;
            }
            // One radius, one width and one color: the uniform shader (most boxes).
            bool uniform = rad[0] == rad[1] && rad[0] == rad[2] && rad[0] == rad[3];
            for (int i = 1; i < 4 && uniform && border; i++)
                uniform = bw[i] == bw[0] && !memcmp(side_color[i], side_color[0], sizeof side_color[0]);
            if (uniform) {
                ui_dl_rect r = {.x = px0, .y = py0, .width = pw, .height = ph, .radius = rad[0],
                                .border_width = border ? bw[0] : 0};
                memcpy(r.color, color, sizeof r.color);
                memcpy(r.border_color, side_color[0], sizeof r.border_color);
                if ((res = ui_display_list_push_rect(dl, &r)) != UI_OK) return res;
            } else {
                ui_dl_border r = {.x = px0, .y = py0, .width = pw, .height = ph};
                memcpy(r.radius, rad, sizeof r.radius);
                for (int i = 0; i < 4; i++) r.border_width[i] = border ? bw[i] : 0;
                memcpy(r.color, color, sizeof r.color);
                memcpy(r.border_color, side_color, sizeof r.border_color);
                if ((res = ui_display_list_push_border(dl, &r)) != UI_OK) return res;
            }
        }
        // What the node's text and children see.
        float inner[4] = {clip[0], clip[1], clip[2], clip[3]};
        if (n->clip) intersect(inner, clip, px0, py0, px1, py1);
        if (n->text && text) {
            // At the anonymous text item's place in the node (layout/layout.h).
            const float *t = n->layout.text;
            float tx = px0 + t[0] * scale, ty = py0 + t[1] * scale;
            if (!move) {
                tx = roundf(tx);
                ty = roundf(ty);
            }
            dl->clip = scissor_of(inner);
            res = build_text(n->text, n->text_color, tx, ty, t[2] * scale, scale, inner, dl, text);
            if (res != UI_OK) return res;
        }
        if (has_children) {
            if (ui_grow((void **)&s->levels, &s->level_capacity, depth + 2, sizeof *s->levels) != UI_OK)
                return UI_ERR_OUT_OF_MEMORY;
            ui_build_level *l = &s->levels[++depth];
            *l = (ui_build_level){slot, x, y, {inner[0], inner[1], inner[2], inner[3]}, move, layer};
            slot = n->first_child;
            continue;
        }
        if (layer && (res = ui_display_list_end_layer(dl)) != UI_OK) return res;
        slot = n->next_sibling;
    }
    return UI_OK;
}
