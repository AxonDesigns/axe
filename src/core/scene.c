#include "core/scene.h"

#include <math.h>
#include <string.h>

#include "core/color.h"
#include "unicode/utf8.h"
#include "wire/wire.h"

#define MODULE "scene"

// Settle thresholds per property, in the property's units.
static const double spring_epsilon[UI_SPRING_COUNT] = {0.01, 0.01, 1e-4};

void ui_scene_init(ui_scene *s, const ui_logger *log) {
    memset(s, 0, sizeof *s);
    s->log = log;
    ui_handle_table_init(&s->handles, UI_HANDLE_NODE);
}

static void free_text(ui_node *n) {
    if (!n->text) return;
    ui_paragraph_free(n->text);
    free(n->text);
    n->text = NULL;
}

void ui_scene_destroy(ui_scene *s) {
    for (uint32_t slot = 0; slot < s->handles.used; slot++) {
        if (s->handles.alive[slot]) free_text(&s->nodes[slot]);
    }
    ui_handle_table_destroy(&s->handles);
    free(s->nodes);
    free(s->scratch);
    free(s->clips);
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
        free_text(&s->nodes[cur]);
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
    s->roots[n->root].dirty = true;
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

static bool apply_op(ui_scene *s, const ui_wire_op *op, uint64_t now_ns) {
    const uint8_t *p = op->payload;
    if (op->opcode == UI_OP_CREATE_NODE) return apply_create(s, p);
    if (op->opcode == UI_OP_SET_TIMER) {
        if (s->timer_fn) s->timer_fn(s->timer_user, ui_wire_get_u32(p), ui_wire_get_u64(p + 8));
        return true;
    }
    // An unknown op comes from a newer writer and may be any size, even shorter than a
    // handle: drop it before reading anything.
    switch (op->opcode) {
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
        ui_color_from_wire(v, n->color);
        n->springs[UI_SPRING_COLOR].active = false;
        return true;
    case UI_OP_SET_CLIP: {
        if (!n) return drop(s, "set_clip", h);
        uint32_t flags = ui_wire_get_u32(p + 8);
        if (flags & ~1u) return drop(s, "set_clip (flags)", h);
        n->clip = flags & 1u;
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
            ui_color_from_wire(v, v);
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
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) s->roots[i].animating = false;
    for (uint32_t slot = 0; slot < s->handles.used; slot++) {
        if (!s->handles.alive[slot]) continue;
        ui_node *n = &s->nodes[slot];
        for (int i = 0; i < UI_SPRING_COUNT; i++) {
            if (!n->springs[i].active) continue;
            ui_spring_advance(&n->springs[i], now_ns, spring_epsilon[i]);
            write_back(n, i);
            s->roots[n->root].dirty = true;
            if (n->springs[i].active) s->roots[n->root].animating = true;
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
static ui_result build_text(ui_paragraph *p, float ox, float oy, float width, float scale,
                            const float clip[4], ui_display_list *dl, const ui_scene_text *t) {
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
            const ui_text_style *st = g->style ? &p->spans[g->style - 1].style : &p->base;
            float a = st->color[3];
            ui_dl_glyph dg = {
                .x = px + ag->left, .y = py + ag->top,
                .width = ag->width, .height = ag->height,
                .u = ag->x, .v = ag->y,
                .color = {st->color[0] * a, st->color[1] * a, st->color[2] * a, a},
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

ui_result ui_scene_build(ui_scene *s, ui_handle surface, ui_display_list *dl,
                         const ui_scene_text *text) {
    ui_scene_root *root = find_root(s, surface);
    if (!root) return UI_ERR_STALE_HANDLE;
    float scale = (float)dl->scale_120 / 120.f;
    // Iterative pre-order walk; origins accumulate down the tree, and so do clips: clip is
    // what applies to the current node, s->clips[depth] what applied to its parent's level.
    uint32_t slot = root->first_child, depth = 0;
    float ox = 0, oy = 0;
    float clip[4] = {-INFINITY, -INFINITY, INFINITY, INFINITY};
    while (slot != UI_NO_SLOT) {
        ui_node *n = &s->nodes[slot];
        float x = ox + n->rect[0], y = oy + n->rect[1];
        float px0 = x * scale, py0 = y * scale;
        float px1 = px0 + n->rect[2] * scale, py1 = py0 + n->rect[3] * scale;
        dl->clip = scissor_of(clip);
        bool visible = px1 + 1 > clip[0] && px0 - 1 < clip[2] && py1 + 1 > clip[1] &&
                       py0 - 1 < clip[3]; // 1 px: the rect shader's anti-aliasing margin
        if (n->rect[2] > 0 && n->rect[3] > 0 && n->color[3] > 0 && visible) {
            float a = n->color[3];
            ui_dl_rect r = {
                .x = px0, .y = py0,
                .width = n->rect[2] * scale, .height = n->rect[3] * scale,
                .radius = n->radius * scale,
                .color = {n->color[0] * a, n->color[1] * a, n->color[2] * a, a},
            };
            ui_result res = ui_display_list_push_rect(dl, &r);
            if (res != UI_OK) return res;
        }
        // What the node's text and children see.
        float inner[4] = {clip[0], clip[1], clip[2], clip[3]};
        if (n->clip) intersect(inner, clip, px0, py0, px1, py1);
        if (n->text && text) {
            dl->clip = scissor_of(inner);
            ui_result res = build_text(n->text, px0, py0, n->rect[2] * scale, scale, inner, dl,
                                       text);
            if (res != UI_OK) return res;
        }
        if (n->first_child != UI_NO_SLOT) {
            if (ui_grow((void **)&s->clips, &s->clip_capacity, depth + 1, sizeof *s->clips) != UI_OK)
                return UI_ERR_OUT_OF_MEMORY;
            memcpy(s->clips[depth++], clip, sizeof clip);
            memcpy(clip, inner, sizeof clip);
            ox = x;
            oy = y;
            slot = n->first_child;
            continue;
        }
        // Climb until a node has a next sibling, restoring the parent origin and clip.
        while (slot != UI_NO_SLOT && s->nodes[slot].next_sibling == UI_NO_SLOT) {
            slot = s->nodes[slot].parent;
            if (slot != UI_NO_SLOT) {
                ox -= s->nodes[slot].rect[0];
                oy -= s->nodes[slot].rect[1];
                memcpy(clip, s->clips[--depth], sizeof clip);
            }
        }
        if (slot != UI_NO_SLOT) slot = s->nodes[slot].next_sibling;
    }
    return UI_OK;
}
