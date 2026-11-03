#include "core/scene.h"

#include <math.h>
#include <string.h>

#include "core/color.h"
#include "style/props.h"
#include "unicode/utf8.h"
#include "wire/wire.h"

#define MODULE "scene"

// Settle thresholds per spring, in the property's units (px, linear color, opacity).
static const double spring_epsilon[UI_SPRING_COUNT] = {0.01, 1e-4, 1e-4, 0.01, 1e-4, 1e-4};
// Components per spring.
static const uint32_t spring_components[UI_SPRING_COUNT] = {4, 4, 4, 1, 1, 4};

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

static void free_node(ui_node *n) {
    if (n->text) {
        ui_paragraph_free(n->text);
        free(n->text);
    }
    free(n->anim);
    free(n->styles);
    free(n->overrides);
    n->text = NULL;
    n->anim = NULL;
    n->styles = NULL;
    n->overrides = NULL;
}

void ui_scene_destroy(ui_scene *s) {
    for (uint32_t slot = 0; slot < s->handles.used; slot++) {
        if (s->handles.alive[slot]) free_node(&s->nodes[slot]);
    }
    ui_handle_table_destroy(&s->handles);
    ui_styles_free(&s->styles);
    free(s->nodes);
    free(s->style_dirty);
    free(s->scratch);
    free(s->levels);
    memset(s, 0, sizeof *s);
}

static ui_scene_root *find_root(ui_scene *s, ui_handle surface) {
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        if (!ui_handle_is_null(s->roots[i].surface) && ui_handle_eq(s->roots[i].surface, surface))
            return &s->roots[i];
    }
    return NULL;
}

ui_result ui_scene_add_root(ui_scene *s, ui_handle surface) {
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        if (ui_handle_is_null(s->roots[i].surface)) {
            s->roots[i] = (ui_scene_root){surface, UI_NO_SLOT, UI_NO_SLOT, false, false, 1.f};
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
static bool destroy_subtree(ui_scene *s, uint32_t slot) {
    if (ui_grow((void **)&s->scratch, &s->scratch_capacity, s->handles.live + 1,
                sizeof *s->scratch) != UI_OK) {
        UI_ERROR(s->log, MODULE, "out of memory destroying a subtree");
        return false;
    }
    unlink(s, slot);
    uint32_t count = 0;
    s->scratch[count++] = slot;
    while (count) {
        uint32_t cur = s->scratch[--count];
        for (uint32_t c = s->nodes[cur].first_child; c != UI_NO_SLOT; c = s->nodes[c].next_sibling)
            s->scratch[count++] = c;
        free_node(&s->nodes[cur]);
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

// Queues a node for style resolution.
static void mark_style_dirty(ui_scene *s, uint32_t slot) {
    ui_node *n = &s->nodes[slot];
    if (n->style_dirty) return;
    if (ui_grow((void **)&s->style_dirty, &s->style_dirty_capacity, s->style_dirty_count + 1,
                sizeof *s->style_dirty) != UI_OK) {
        UI_ERROR(s->log, MODULE, "out of memory queueing a style resolution");
        return;
    }
    n->style_dirty = true;
    s->style_dirty[s->style_dirty_count++] = slot;
}

static ui_node_anim *anim_of(ui_node *n) {
    if (!n->anim) n->anim = calloc(1, sizeof *n->anim);
    return n->anim;
}

// Sets (or with UI_VALUE_UNSET removes) the node's override of one property.
static bool set_override(ui_node *n, uint16_t prop, const ui_value *v) {
    for (uint32_t i = 0; i < n->override_count; i++) {
        if (n->overrides[i].prop != prop) continue;
        if (v->kind == UI_VALUE_UNSET) n->overrides[i] = n->overrides[--n->override_count];
        else n->overrides[i].value = *v;
        return true;
    }
    if (v->kind == UI_VALUE_UNSET) return true;
    uint32_t cap = n->override_capacity;
    if (ui_grow((void **)&n->overrides, &cap, n->override_count + 1u, sizeof *n->overrides) != UI_OK)
        return false;
    n->override_capacity = (uint16_t)(cap > UINT16_MAX ? UINT16_MAX : cap);
    n->overrides[n->override_count++] = (ui_style_entry){prop, *v};
    return true;
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
    mark_style_dirty(s, slot);
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
    // Every other known op addresses one node; a change marks that node's surface dirty.
    ui_handle h = ui_wire_get_handle(p);
    ui_node *n = node_of(s, h);
    uint32_t root = n ? n->root : 0;
    bool applied = apply_node_op(s, op, now_ns);
    if (applied && n) s->roots[root].dirty = true;
    return applied;
}

// A set_* op on a paint property: the override changes, and the value jumps unless the
// styles declare a transition for it.
static bool set_paint(ui_scene *s, ui_node *n, uint16_t prop, ui_value v, int spring) {
    if (!set_override(n, prop, &v)) return false;
    if (spring >= 0) n->jump |= (uint8_t)(1u << spring);
    mark_style_dirty(s, (uint32_t)(n - s->nodes));
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
    mark_style_dirty(s, (uint32_t)(n - s->nodes));
    return true;
}

static bool apply_set_props(ui_scene *s, ui_node *n, const uint8_t *p, uint32_t size) {
    uint32_t header = ui_wire_set_props_header(p), count = ui_wire_get_u16(p + 10), decoded;
    ui_style_entry local[16], *e = count <= 16 ? local : malloc(count * sizeof *e);
    if (!e) return false;
    bool ok = ui_style_decode(p + header, size - header, count, true, e, &decoded);
    for (uint32_t i = 0; ok && i < decoded; i++) ok = set_override(n, e[i].prop, &e[i].value);
    if (e != local) free(e);
    if (ok) mark_style_dirty(s, (uint32_t)(n - s->nodes));
    return ok;
}

static bool apply_node_op(ui_scene *s, const ui_wire_op *op, uint64_t now_ns) {
    (void)now_ns; // springs start when styles are resolved
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
        memcpy(n->rect, v, sizeof n->rect);
        if (n->anim) n->anim->springs[UI_SPRING_RECT].active = false;
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
            ui_spring_start(&a->springs[UI_SPRING_RECT], count, n->rect, v, stiffness, damping, now_ns);
            return true;
        }
        // Paint properties: the override becomes the target, and the spring starts toward
        // whatever that resolves to, with these parameters.
        bool ok = prop == UI_PROP_COLOR
                      ? set_override(n, UI_STYLE_BACKGROUND_COLOR, &(ui_value){.kind = UI_VALUE_COLOR, .color = {v[0], v[1], v[2], v[3]}})
                      : set_override(n, UI_STYLE_CORNER_RADIUS, &(ui_value){.kind = UI_VALUE_PX, .number = v[0]});
        if (!ok) return drop(s, "start_spring (out of memory)", h);
        int i = prop == UI_PROP_COLOR ? UI_SPRING_COLOR : UI_SPRING_RADIUS;
        a->requested[i] = true;
        a->requested_params[i][0] = stiffness;
        a->requested_params[i][1] = damping;
        n->jump &= (uint8_t)~(1u << i);
        mark_style_dirty(s, (uint32_t)(n - s->nodes));
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
    case UI_OP_SET_STATE: {
        if (!n) return drop(s, "set_state", h);
        uint32_t mask = ui_wire_get_u32(p + 8), value = ui_wire_get_u32(p + 12);
        uint8_t state = (uint8_t)((n->state & ~mask) | (value & mask));
        if (state != n->state) mark_style_dirty(s, (uint32_t)(n - s->nodes));
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
    for (uint32_t i = 0; prep && i < prep->scale_count; i++) {
        if (prep->scales[i] != s->roots[n->root].scale) continue;
        ui_paragraph_adopt_shaped(n->text, &prep->shaped[i]);
        break;
    }
    s->stats.text_invalid += t->replaced;
    if (t->replaced)
        UI_DEBUG_REPORT(s->log, MODULE, "set_text: replaced %u ill-formed UTF-8 sequence(s)", t->replaced);
    ui_text_op_free(t);
    s->roots[n->root].dirty = true;
    mark_style_dirty(s, (uint32_t)(n - s->nodes)); // text color and paragraph values follow styles
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
    case UI_SPRING_RECT: return n->rect;
    case UI_SPRING_COLOR: return n->color;
    case UI_SPRING_BORDER_COLOR: return n->border_color;
    case UI_SPRING_RADIUS: return &n->radius;
    case UI_SPRING_OPACITY: return &n->opacity;
    default: return n->text_color;
    }
}

static void write_back(ui_node *n, int which) {
    ui_spring *sp = &n->anim->springs[which];
    float *dst = presented(n, which);
    for (uint32_t i = 0; i < sp->components; i++) dst[i] = (float)sp->position[i];
    // Springs may overshoot; clamp what is shown, not the spring state.
    if (which == UI_SPRING_RECT) {
        n->rect[2] = fmaxf(n->rect[2], 0.f);
        n->rect[3] = fmaxf(n->rect[3], 0.f);
    } else if (which == UI_SPRING_RADIUS) {
        n->radius = fmaxf(n->radius, 0.f);
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

static void resolve_node(ui_scene *s, ui_node *n, uint64_t now_ns) {
    ui_computed *c = &n->computed;
    ui_style_resolve(&s->styles, n->styles, n->style_count, n->state, n->overrides,
                     n->override_count, c);
    // The text color follows the styles when they set one, else the text's own.
    float text_target[4];
    const float *text_src = c->text_set & UI_TEXT_SET_COLOR ? c->text_color
                            : n->text                       ? n->text->base.color
                                                            : n->text_color;
    memcpy(text_target, text_src, sizeof text_target);
    bool changed = false;
    changed |= update_paint(n, UI_SPRING_COLOR, c->background, c->transition[UI_ANIM_BACKGROUND], now_ns);
    changed |= update_paint(n, UI_SPRING_BORDER_COLOR, c->border_color,
                            c->transition[UI_ANIM_BORDER_COLOR], now_ns);
    changed |= update_paint(n, UI_SPRING_RADIUS, &c->corner_radius, c->transition[UI_ANIM_RADIUS], now_ns);
    changed |= update_paint(n, UI_SPRING_OPACITY, &c->opacity, c->transition[UI_ANIM_OPACITY], now_ns);
    changed |= update_paint(n, UI_SPRING_TEXT_COLOR, text_target, c->transition[UI_ANIM_TEXT_COLOR], now_ns);
    changed |= n->clip != c->clip || n->border_width != c->border_width;
    n->clip = c->clip;
    n->border_width = c->border_width;
    if (n->text) {
        const ui_text_source *src = n->text->source;
        uint32_t lines = c->text_set & UI_TEXT_SET_MAX_LINES ? c->max_lines : src ? src->max_lines : 0;
        ui_align align = c->text_set & UI_TEXT_SET_ALIGN ? (ui_align)c->text_align
                         : src                           ? src->align
                                                         : UI_ALIGN_START;
        float lh = c->text_set & UI_TEXT_SET_LINE_HEIGHT ? c->line_height : src ? src->line_height : 0;
        bool valid = n->text->layout_valid;
        ui_paragraph_set_params(n->text, lines, align, lh);
        changed |= valid != n->text->layout_valid;
    }
    n->jump = 0;
    n->resolved = true;
    if (changed) s->roots[n->root].dirty = true;
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
            if (uses) mark_style_dirty(s, slot);
        }
    }
    free(styles);
    free(tokens);
    for (uint32_t i = 0; i < s->style_dirty_count; i++) {
        uint32_t slot = s->style_dirty[i];
        if (slot >= s->handles.used || !s->handles.alive[slot] || !s->nodes[slot].style_dirty) continue;
        s->nodes[slot].style_dirty = false;
        resolve_node(s, &s->nodes[slot], now_ns);
    }
    s->style_dirty_count = 0;
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
            write_back(n, i);
            s->roots[n->root].dirty = true;
            if (sp->active) s->roots[n->root].animating = true;
        }
    }
}

ui_scene_root *ui_scene_root_of(ui_scene *s, ui_handle surface) { return find_root(s, surface); }

ui_node *ui_scene_node(ui_scene *s, ui_handle h) { return node_of(s, h); }

ui_handle ui_scene_handle_of(const ui_scene *s, uint32_t slot) { return handle_of(s, slot); }

void ui_scene_fonts_changed(ui_scene *s) {
    for (uint32_t slot = 0; slot < s->handles.used; slot++) {
        if (s->handles.alive[slot] && s->nodes[slot].text) s->roots[s->nodes[slot].root].dirty = true;
    }
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
            const float *c = g->style ? p->spans[g->style - 1].style.color : base_color;
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

// A node moves while its rect springs; then it, and everything under it, draws where it is
// instead of snapping to whole pixels.
static bool moving(const ui_node *n) { return n->anim && n->anim->springs[UI_SPRING_RECT].active; }

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
        if (!(n->opacity > 0)) { // invisible, with its subtree
            slot = n->next_sibling;
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
        float bw = border_px(n->border_width, scale, !move);
        bool border = bw > 0 && n->border_color[3] > 0;
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
            float a = n->color[3] * alpha, ba = border ? n->border_color[3] * alpha : 0;
            ui_dl_rect r = {
                .x = px0, .y = py0, .width = pw, .height = ph,
                .radius = n->radius * scale,
                .border_width = border ? bw : 0,
                .color = {n->color[0] * a, n->color[1] * a, n->color[2] * a, a},
                .border_color = {n->border_color[0] * ba, n->border_color[1] * ba,
                                 n->border_color[2] * ba, ba},
            };
            if ((res = ui_display_list_push_rect(dl, &r)) != UI_OK) return res;
        }
        // What the node's text and children see.
        float inner[4] = {clip[0], clip[1], clip[2], clip[3]};
        if (n->clip) intersect(inner, clip, px0, py0, px1, py1);
        if (n->text && text) {
            dl->clip = scissor_of(inner);
            res = build_text(n->text, n->text_color, px0, py0, n->rect[2] * scale, scale, inner,
                             dl, text);
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
