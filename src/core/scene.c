#include "core/scene.h"

#include <math.h>
#include <string.h>

#include "wire/wire.h"

#define MODULE "scene"

// Settle thresholds per property, in the property's units.
static const double spring_epsilon[UI_SPRING_COUNT] = {0.01, 0.01, 1e-4};

static float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

void ui_scene_init(ui_scene *s, const ui_logger *log) {
    memset(s, 0, sizeof *s);
    s->log = log;
    ui_handle_table_init(&s->handles, UI_HANDLE_NODE);
}

void ui_scene_destroy(ui_scene *s) {
    ui_handle_table_destroy(&s->handles);
    free(s->nodes);
    free(s->scratch);
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
            s->roots[i] = (ui_scene_root){surface, UI_NO_SLOT, UI_NO_SLOT};
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

// Checks the framing of every op without applying anything.
static ui_result validate(const uint8_t *data, size_t size) {
    ui_wire_reader r;
    ui_result res = ui_wire_reader_init(&r, UI_WIRE_MAGIC_CMD, data, size);
    if (res != UI_OK) return res;
    ui_wire_op op;
    while ((res = ui_wire_reader_next(&r, &op)) == UI_OK) {
        uint32_t expected;
        switch (op.opcode) {
        case UI_OP_CREATE_NODE: expected = UI_WIRE_SIZE_CREATE_NODE; break;
        case UI_OP_DESTROY_NODE: expected = UI_WIRE_SIZE_DESTROY_NODE; break;
        case UI_OP_SET_RECT: expected = UI_WIRE_SIZE_SET_RECT; break;
        case UI_OP_SET_CORNER_RADIUS: expected = UI_WIRE_SIZE_SET_CORNER_RADIUS; break;
        case UI_OP_SET_COLOR: expected = UI_WIRE_SIZE_SET_COLOR; break;
        case UI_OP_START_SPRING:
            if (op.payload_size < UI_WIRE_SIZE_START_SPRING_BASE) return UI_ERR_MALFORMED;
            expected = UI_WIRE_SIZE_START_SPRING_BASE + 4u * ui_wire_get_u16(op.payload + 10);
            break;
        default: continue; // unknown ops are skipped when applying
        }
        if (op.payload_size != expected) return UI_ERR_MALFORMED;
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
    return true;
}

static bool apply_op(ui_scene *s, const ui_wire_op *op, uint64_t now_ns) {
    const uint8_t *p = op->payload;
    if (op->opcode == UI_OP_CREATE_NODE) return apply_create(s, p);

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
        n->springs[UI_SPRING_RECT].active = false;
        return true;
    case UI_OP_SET_CORNER_RADIUS:
        if (!n) return drop(s, "set_corner_radius", h);
        v[0] = ui_wire_get_f32(p + 8);
        if (!isfinite(v[0]) || v[0] < 0) return drop(s, "set_corner_radius (value)", h);
        n->radius = v[0];
        n->springs[UI_SPRING_RADIUS].active = false;
        return true;
    case UI_OP_SET_COLOR:
        if (!n) return drop(s, "set_color", h);
        for (int i = 0; i < 4; i++) v[i] = ui_wire_get_f32(p + 8 + 4 * i);
        if (!finite_n(v, 4)) return drop(s, "set_color (value)", h);
        for (int i = 0; i < 3; i++) n->color[i] = srgb_to_linear(fminf(fmaxf(v[i], 0.f), 1.f));
        n->color[3] = fminf(fmaxf(v[3], 0.f), 1.f);
        n->springs[UI_SPRING_COLOR].active = false;
        return true;
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
        ui_spring *spring;
        const float *current;
        switch (prop) {
        case UI_PROP_RECT:
            spring = &n->springs[UI_SPRING_RECT];
            current = n->rect;
            break;
        case UI_PROP_CORNER_RADIUS:
            spring = &n->springs[UI_SPRING_RADIUS];
            current = &n->radius;
            break;
        case UI_PROP_COLOR:
            spring = &n->springs[UI_SPRING_COLOR];
            current = n->color;
            for (int i = 0; i < 3; i++) v[i] = srgb_to_linear(fminf(fmaxf(v[i], 0.f), 1.f));
            v[3] = fminf(fmaxf(v[3], 0.f), 1.f);
            break;
        default: return drop(s, "start_spring (property)", h);
        }
        ui_spring_start(spring, count, current, v, stiffness, damping, now_ns);
        return true;
    }
    default:
        s->stats.dropped++;
        UI_DEBUG_REPORT(s->log, MODULE, "dropped unknown op %u", op->opcode);
        return false;
    }
}

ui_result ui_scene_apply(ui_scene *s, const uint8_t *data, size_t size, uint64_t now_ns) {
    ui_result res = validate(data, size);
    if (res != UI_OK) {
        s->stats.rejected++;
        UI_DEBUG_REPORT(s->log, MODULE, "rejected malformed command buffer (%zu bytes)", size);
        return res;
    }
    ui_wire_reader r;
    ui_wire_reader_init(&r, UI_WIRE_MAGIC_CMD, data, size);
    ui_wire_op op;
    while (ui_wire_reader_next(&r, &op) == UI_OK) {
        if (apply_op(s, &op, now_ns)) s->stats.applied++;
    }
    return UI_OK;
}

static void write_back(ui_node *n, int which) {
    ui_spring *sp = &n->springs[which];
    float *dst = which == UI_SPRING_RECT ? n->rect : which == UI_SPRING_RADIUS ? &n->radius : n->color;
    for (uint32_t i = 0; i < sp->components; i++) dst[i] = (float)sp->position[i];
    if (which == UI_SPRING_RECT) {
        // Springs may overshoot below zero size; clamp what is shown, not the spring state.
        n->rect[2] = fmaxf(n->rect[2], 0.f);
        n->rect[3] = fmaxf(n->rect[3], 0.f);
    } else if (which == UI_SPRING_RADIUS) {
        n->radius = fmaxf(n->radius, 0.f);
    } else {
        for (int i = 0; i < 4; i++) n->color[i] = fminf(fmaxf(n->color[i], 0.f), 1.f);
    }
}

void ui_scene_animate(ui_scene *s, uint64_t now_ns) {
    for (uint32_t slot = 0; slot < s->handles.used; slot++) {
        if (!s->handles.alive[slot]) continue;
        ui_node *n = &s->nodes[slot];
        for (int i = 0; i < UI_SPRING_COUNT; i++) {
            if (!n->springs[i].active) continue;
            ui_spring_advance(&n->springs[i], now_ns, spring_epsilon[i]);
            write_back(n, i);
        }
    }
}

bool ui_scene_animating(const ui_scene *s) {
    for (uint32_t slot = 0; slot < s->handles.used; slot++) {
        if (!s->handles.alive[slot]) continue;
        for (int i = 0; i < UI_SPRING_COUNT; i++) {
            if (s->nodes[slot].springs[i].active) return true;
        }
    }
    return false;
}

ui_result ui_scene_build(ui_scene *s, ui_handle surface, ui_display_list *dl) {
    ui_scene_root *root = find_root(s, surface);
    if (!root) return UI_ERR_STALE_HANDLE;
    float scale = (float)dl->scale_120 / 120.f;
    // Iterative pre-order walk; origins accumulate down the tree.
    uint32_t slot = root->first_child;
    float ox = 0, oy = 0;
    while (slot != UI_NO_SLOT) {
        ui_node *n = &s->nodes[slot];
        float x = ox + n->rect[0], y = oy + n->rect[1];
        if (n->rect[2] > 0 && n->rect[3] > 0 && n->color[3] > 0) {
            float a = n->color[3];
            ui_dl_rect r = {
                .x = x * scale, .y = y * scale,
                .width = n->rect[2] * scale, .height = n->rect[3] * scale,
                .radius = n->radius * scale,
                .color = {n->color[0] * a, n->color[1] * a, n->color[2] * a, a},
            };
            ui_result res = ui_display_list_push_rect(dl, &r);
            if (res != UI_OK) return res;
        }
        if (n->first_child != UI_NO_SLOT) {
            ox = x;
            oy = y;
            slot = n->first_child;
            continue;
        }
        // Climb until a node has a next sibling, restoring the parent origin on the way.
        while (slot != UI_NO_SLOT && s->nodes[slot].next_sibling == UI_NO_SLOT) {
            slot = s->nodes[slot].parent;
            if (slot != UI_NO_SLOT) {
                ox -= s->nodes[slot].rect[0];
                oy -= s->nodes[slot].rect[1];
            }
        }
        if (slot != UI_NO_SLOT) slot = s->nodes[slot].next_sibling;
    }
    return UI_OK;
}
