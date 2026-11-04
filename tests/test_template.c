// Templates (docs/templates.md): an instance expands to exactly the ops of its template with
// its handles, byte for byte against the same ops written by hand, for fixed and for random
// templates; both apply with nothing dropped and resolve as the full recomputation does.
// Invalid templates and instances are dropped; redefinition and destruction; buffers without
// templates are left alone.
#include <stdlib.h>
#include <string.h>

#include "core/scene.h"
#include "core/template.h"
#include "wire/gen/wire_gen.h"
#include "wire/wire.h"
#include "test.h"
#include "ui/ui.h"

static ui_logger quiet = {UI_LOG_ERROR};
static const ui_handle NUL = {0, 0};
static const ui_handle SURFACE = {(uint32_t)UI_HANDLE_SURFACE << UI_HANDLE_SLOT_BITS, 1};

static uint8_t *take(ui_cmd_writer *w, size_t *size) {
    uint8_t *bytes;
    CHECK(ui_cmd_writer_take(w, &bytes, size) == UI_OK);
    return bytes;
}

// ---- A random template --------------------------------------------------------------------

typedef struct rng {
    uint64_t s;
} rng;

static uint32_t next(rng *r, uint32_t n) {
    r->s = r->s * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(r->s >> 33) % n;
}

typedef struct shape {
    uint32_t count;
    int32_t parent[64]; // -1: a root
    uint32_t ops[64];   // which extra ops node i gets (bits)
    uint32_t order[64]; // creation order is index order; ops follow each create
} shape;

enum { OP_STYLES = 1, OP_PROPS = 2, OP_TOKENS = 4, OP_TEXT = 8, OP_STATE = 16 };

static shape random_shape(rng *r) {
    shape s = {.count = 1 + next(r, 40)};
    for (uint32_t i = 0; i < s.count; i++) {
        s.parent[i] = i == 0 || next(r, 5) == 0 ? -1 : (int32_t)next(r, i);
        s.ops[i] = next(r, 32);
    }
    return s;
}

static const char *const families[] = {"Noto Sans, sans-serif"};

// The ops of `s` with handles h[]; plain: roots created under `parent` and moved before
// `before`, as the expansion must write them; otherwise roots get a null parent.
static void write_shape(ui_cmd_writer *w, const shape *s, const ui_handle *h, bool plain, ui_handle parent,
                        ui_handle before) {
    for (uint32_t i = 0; i < s->count; i++) {
        bool root = s->parent[i] < 0;
        ui_handle p = root ? (plain ? parent : NUL) : h[s->parent[i]];
        CHECK(ui_cmd_create_node(w, h[i], p) == UI_OK);
        if (root && plain && !ui_handle_is_null(before)) CHECK(ui_cmd_move_node(w, h[i], parent, before) == UI_OK);
        uint32_t ops = s->ops[i];
        if (ops & OP_STYLES) CHECK(ui_cmd_set_styles(w, h[i], (uint32_t[]){1 + i % 3, 7}, 2) == UI_OK);
        if (ops & OP_PROPS) {
            ui_style_decl d[] = {{UI_STYLE_WIDTH, ui_px(10.f + (float)i)},
                                 {UI_STYLE_BACKGROUND_COLOR, ui_rgba(0.1f * (float)(i % 10), 0.5f, 0.2f, 1)}};
            CHECK(ui_cmd_set_props(w, h[i], d, 2) == UI_OK);
        }
        if (ops & OP_TOKENS) {
            ui_token_decl d[] = {{3, ui_rgba(1, 0, 0, 1)}};
            CHECK(ui_cmd_set_token_overrides(w, h[i], d, 1) == UI_OK);
        }
        if (ops & OP_TEXT) {
            const char *text = (i % 2) ? "Hello, template" : "Ünïcödé text";
            ui_text_span_desc span = {.start = 0, .end = 3, .style = {.weight = 700}, .set = UI_TEXT_SET_WEIGHT};
            ui_text_desc d = {.text = text, .length = (uint32_t)strlen(text), .spans = &span, .span_count = i % 3 == 0,
                              .families = families, .family_count = 1, .style = {.family = 0, .size = 14},
                              .set = UI_TEXT_SET_FAMILY | UI_TEXT_SET_SIZE};
            CHECK(ui_cmd_set_text(w, h[i], &d) == UI_OK);
        }
        if (ops & OP_STATE) CHECK(ui_cmd_set_state(w, h[i], UI_STATE_SELECTED, UI_STATE_SELECTED) == UI_OK);
    }
}

// A scene with a surface, some styles and tokens, and two existing root-level nodes (slots 1
// and 2) to place instances before.
static void scene_setup(ui_scene *s) {
    ui_scene_init(s, &quiet);
    ui_scene_add_root(s, SURFACE);
    ui_cmd_writer *w;
    ui_cmd_writer_create(&w);
    for (uint32_t id = 1; id <= 7; id++) {
        ui_style_decl d = {UI_STYLE_PADDING_TOP, ui_px((float)id)};
        ui_cmd_define_style(w, id, &(ui_style_block_desc){.decls = &d, .count = 1}, 1);
    }
    ui_cmd_set_token(w, 3, ui_rgba(0, 0, 1, 1));
    ui_cmd_create_node(w, (ui_handle){1, 1}, SURFACE);
    ui_cmd_create_node(w, (ui_handle){2, 1}, SURFACE);
    size_t size;
    uint8_t *bytes = take(w, &size);
    CHECK(ui_scene_apply(s, bytes, size, 0) == UI_OK);
    free(bytes);
    ui_cmd_writer_destroy(w);
}

static void apply_resolved(ui_scene *s, const uint8_t *bytes, size_t size) {
    CHECK(ui_scene_apply(s, bytes, size, 0) == UI_OK);
    ui_scene_resolve(s, 0);
    CHECK(ui_scene_check_resolved(s)); // as resolving every node from scratch
}

// Expands [define, instance] and checks it equals [define, the same ops by hand], then that both
// build the same tree in a scene, with nothing dropped.
static void check_equivalent(const shape *sh, ui_handle parent, ui_handle before, uint32_t first_slot) {
    ui_handle local[64], real[64];
    for (uint32_t i = 0; i < sh->count; i++) {
        local[i] = ui_template_node(i);
        real[i] = (ui_handle){first_slot + i, 1};
    }
    ui_cmd_writer *w, *ops;
    ui_cmd_writer_create(&w);
    ui_cmd_writer_create(&ops);
    write_shape(ops, sh, local, false, NUL, NUL);
    CHECK(ui_cmd_define_template(w, 9, sh->count, ops) == UI_OK);
    CHECK_EQ_INT(ui_cmd_writer_op_count(ops), 0);
    CHECK(ui_cmd_instantiate_template(w, 9, parent, before, real, sh->count) == UI_OK);
    size_t a_size;
    uint8_t *a = take(w, &a_size);

    write_shape(ops, sh, local, false, NUL, NUL);
    CHECK(ui_cmd_define_template(w, 9, sh->count, ops) == UI_OK);
    write_shape(w, sh, real, true, parent, before);
    size_t b_size;
    uint8_t *b = take(w, &b_size);

    ui_templates *t;
    CHECK(ui_templates_create(&quiet, &t) == UI_OK);
    uint8_t *x;
    size_t x_size;
    CHECK(ui_templates_expand(t, a, a_size, &x, &x_size) == UI_OK);
    CHECK(x != NULL);
    CHECK_EQ_INT(x_size, b_size);
    CHECK(x && x_size == b_size && memcmp(x, b, b_size) == 0);
    CHECK_EQ_INT(ui_templates_stats(t).instances, 1);
    CHECK_EQ_INT(ui_templates_stats(t).dropped, 0);

    ui_scene s1, s2;
    scene_setup(&s1);
    scene_setup(&s2);
    if (x) apply_resolved(&s1, x, x_size);
    apply_resolved(&s2, b, b_size);
    CHECK_EQ_INT(s1.stats.dropped, 0);
    CHECK_EQ_INT(s2.stats.dropped, 0);
    for (uint32_t i = 0; i < sh->count; i++) {
        const ui_node *n1 = &s1.nodes[first_slot + i], *n2 = &s2.nodes[first_slot + i];
        CHECK_EQ_INT(n1->parent, n2->parent);
        CHECK_EQ_INT(n1->next_sibling, n2->next_sibling);
        CHECK(memcmp(n1->color, n2->color, sizeof n1->color) == 0);
    }
    ui_scene_destroy(&s1);
    ui_scene_destroy(&s2);
    ui_templates_destroy(t);
    free(x);
    free(a);
    free(b);
    ui_cmd_writer_destroy(ops);
    ui_cmd_writer_destroy(w);
}

static void test_equivalence_fixed(void) {
    // A card: a root with a title and a body, and a second root after it.
    shape card = {.count = 4, .parent = {-1, 0, 0, -1}, .ops = {OP_STYLES | OP_STATE, OP_TEXT | OP_PROPS, OP_TOKENS | OP_TEXT, OP_TEXT}};
    check_equivalent(&card, SURFACE, NUL, 10);
    check_equivalent(&card, SURFACE, (ui_handle){2, 1}, 10); // before an existing root-level node
    check_equivalent(&card, (ui_handle){1, 1}, NUL, 10);      // under a node
}

static void test_equivalence_random(void) {
    rng r = {42};
    for (int i = 0; i < 300; i++) {
        shape s = random_shape(&r);
        ui_handle before = next(&r, 2) ? (ui_handle){2, 1} : NUL;
        check_equivalent(&s, SURFACE, before, 10 + next(&r, 100));
    }
}

// ---- Rules ----------------------------------------------------------------------------------

// Defines template 5 from `ops` and instantiates it; returns what was dropped.
static uint64_t dropped_for(void (*write)(ui_cmd_writer *), uint32_t node_count) {
    ui_cmd_writer *w, *ops;
    ui_cmd_writer_create(&w);
    ui_cmd_writer_create(&ops);
    write(ops);
    ui_cmd_define_template(w, 5, node_count, ops);
    ui_cmd_instantiate_template(w, 5, SURFACE, NUL, (ui_handle[]){{10, 1}, {11, 1}}, node_count);
    size_t size;
    uint8_t *bytes = take(w, &size);
    ui_templates *t;
    ui_templates_create(&quiet, &t);
    uint8_t *x;
    size_t x_size;
    CHECK(ui_templates_expand(t, bytes, size, &x, &x_size) == UI_OK);
    uint64_t dropped = ui_templates_stats(t).dropped;
    ui_templates_destroy(t);
    free(x);
    free(bytes);
    ui_cmd_writer_destroy(ops);
    ui_cmd_writer_destroy(w);
    return dropped;
}

static void good(ui_cmd_writer *w) {
    ui_cmd_create_node(w, ui_template_node(0), NUL);
    ui_cmd_create_node(w, ui_template_node(1), ui_template_node(0));
}
static void created_twice(ui_cmd_writer *w) {
    good(w);
    ui_cmd_create_node(w, ui_template_node(1), ui_template_node(0));
}
static void parent_later(ui_cmd_writer *w) {
    ui_cmd_create_node(w, ui_template_node(1), ui_template_node(0));
    ui_cmd_create_node(w, ui_template_node(0), NUL);
}
static void named_before(ui_cmd_writer *w) {
    ui_cmd_set_state(w, ui_template_node(1), 1, 1);
    good(w);
}
static void missing_node(ui_cmd_writer *w) { ui_cmd_create_node(w, ui_template_node(0), NUL); }
static void outside(ui_cmd_writer *w) {
    good(w);
    ui_cmd_set_state(w, (ui_handle){5, 1}, 1, 1); // not one of its two nodes
}
static void real_handle(ui_cmd_writer *w) {
    ui_cmd_create_node(w, ui_template_node(0), NUL);
    ui_cmd_create_node(w, (ui_handle){1, 2}, ui_template_node(0)); // generation 2: not local
}
static void wrong_op(ui_cmd_writer *w) {
    good(w);
    ui_cmd_destroy_node(w, ui_template_node(1));
}
static void foreign_parent(ui_cmd_writer *w) {
    ui_cmd_create_node(w, ui_template_node(0), SURFACE); // a root names no parent
    ui_cmd_create_node(w, ui_template_node(1), ui_template_node(0));
}

static void test_rules(void) {
    CHECK_EQ_INT(dropped_for(good, 2), 0);
    // An invalid template is dropped, and so is its instance (no such template).
    CHECK_EQ_INT(dropped_for(created_twice, 2), 2);
    CHECK_EQ_INT(dropped_for(parent_later, 2), 2);
    CHECK_EQ_INT(dropped_for(named_before, 2), 2);
    CHECK_EQ_INT(dropped_for(missing_node, 2), 2);
    CHECK_EQ_INT(dropped_for(outside, 2), 2);
    CHECK_EQ_INT(dropped_for(real_handle, 2), 2);
    CHECK_EQ_INT(dropped_for(wrong_op, 2), 2);
    CHECK_EQ_INT(dropped_for(foreign_parent, 2), 2);
    // The writer refuses what can't be framed.
    ui_cmd_writer *w, *ops;
    ui_cmd_writer_create(&w);
    ui_cmd_writer_create(&ops);
    CHECK(ui_cmd_define_template(w, 0, 1, ops) == UI_ERR_INVALID_ARGUMENT);
    CHECK(ui_cmd_define_template(w, 1, 0, ops) == UI_ERR_INVALID_ARGUMENT);
    CHECK(ui_cmd_define_template(w, 1, 1025, ops) == UI_ERR_INVALID_ARGUMENT);
    CHECK(ui_cmd_define_template(w, 1, 1, w) == UI_ERR_INVALID_ARGUMENT);
    CHECK(ui_cmd_instantiate_template(w, 1, SURFACE, NUL, (ui_handle[]){{1, 1}}, 0) == UI_ERR_INVALID_ARGUMENT);
    CHECK(ui_cmd_destroy_template(w, 0) == UI_ERR_INVALID_ARGUMENT);
    ui_cmd_writer_destroy(ops);
    ui_cmd_writer_destroy(w);
}

// Expands what `w` holds with `t`, returns the number of create_node ops in the result.
static uint32_t creates(ui_templates *t, ui_cmd_writer *w) {
    size_t size, x_size;
    uint8_t *bytes = take(w, &size), *x;
    CHECK(ui_templates_expand(t, bytes, size, &x, &x_size) == UI_OK);
    uint32_t n = 0;
    ui_wire_reader r;
    ui_wire_op op;
    ui_wire_reader_init(&r, UI_WIRE_MAGIC_CMD, x ? x : bytes, x ? x_size : size);
    while (ui_wire_reader_next(&r, &op) == UI_OK) n += op.opcode == UI_OP_CREATE_NODE;
    free(x);
    free(bytes);
    return n;
}

static void test_redefine_destroy(void) {
    ui_templates *t;
    ui_templates_create(&quiet, &t);
    ui_cmd_writer *w, *ops;
    ui_cmd_writer_create(&w);
    ui_cmd_writer_create(&ops);
    ui_cmd_create_node(ops, ui_template_node(0), NUL);
    ui_cmd_define_template(w, 3, 1, ops);
    CHECK_EQ_INT(creates(t, w), 0); // defined, nothing instantiated
    // Instances in a later buffer use the definition known then.
    ui_cmd_instantiate_template(w, 3, SURFACE, NUL, (ui_handle[]){{10, 1}}, 1);
    CHECK_EQ_INT(creates(t, w), 1);
    // Redefined with two nodes: one handle no longer matches.
    good(ops);
    ui_cmd_define_template(w, 3, 2, ops);
    ui_cmd_instantiate_template(w, 3, SURFACE, NUL, (ui_handle[]){{11, 1}}, 1);
    ui_cmd_instantiate_template(w, 3, SURFACE, NUL, (ui_handle[]){{12, 1}, {13, 1}}, 2);
    CHECK_EQ_INT(creates(t, w), 2);
    CHECK_EQ_INT(ui_templates_stats(t).dropped, 1);
    // Destroyed: later instances are dropped; an invalid redefinition keeps the old one.
    ui_cmd_destroy_template(w, 3);
    ui_cmd_instantiate_template(w, 3, SURFACE, NUL, (ui_handle[]){{14, 1}, {15, 1}}, 2);
    CHECK_EQ_INT(creates(t, w), 0);
    CHECK_EQ_INT(ui_templates_stats(t).destroyed, 1);
    good(ops);
    ui_cmd_define_template(w, 4, 2, ops);
    created_twice(ops);
    ui_cmd_define_template(w, 4, 2, ops);
    ui_cmd_instantiate_template(w, 4, SURFACE, NUL, (ui_handle[]){{16, 1}, {17, 1}}, 2);
    CHECK_EQ_INT(creates(t, w), 2);
    // Many templates: the map grows and keeps them all.
    for (uint32_t id = 100; id < 400; id++) {
        ui_cmd_create_node(ops, ui_template_node(0), NUL);
        ui_cmd_define_template(w, id, 1, ops);
        if (id % 3 == 0) ui_cmd_destroy_template(w, id);
    }
    for (uint32_t id = 100; id < 400; id++) ui_cmd_instantiate_template(w, id, SURFACE, NUL, (ui_handle[]){{100 + id, 1}}, 1);
    CHECK_EQ_INT(creates(t, w), 200);
    ui_cmd_writer_destroy(ops);
    ui_cmd_writer_destroy(w);
    ui_templates_destroy(t);
}

static void test_plain_buffers_untouched(void) {
    ui_templates *t;
    ui_templates_create(&quiet, &t);
    ui_cmd_writer *w;
    ui_cmd_writer_create(&w);
    ui_cmd_create_node(w, (ui_handle){1, 1}, SURFACE);
    size_t size, x_size;
    uint8_t *bytes = take(w, &size), *x;
    CHECK(ui_templates_expand(t, bytes, size, &x, &x_size) == UI_OK);
    CHECK(x == NULL);
    free(bytes);
    // A malformed buffer is left for the scene to reject.
    uint8_t junk[16] = {1, 2, 3};
    CHECK(ui_templates_expand(t, junk, sizeof junk, &x, &x_size) == UI_OK);
    CHECK(x == NULL);
    // A buffer of dropped instances only: an empty, valid buffer.
    ui_cmd_instantiate_template(w, 77, SURFACE, NUL, (ui_handle[]){{1, 1}}, 1);
    bytes = take(w, &size);
    CHECK(ui_templates_expand(t, bytes, size, &x, &x_size) == UI_OK);
    CHECK(x != NULL && x_size == UI_WIRE_HEADER_SIZE);
    CHECK(x && ui_wire_validate_commands(x, x_size) == UI_OK);
    free(x);
    free(bytes);
    // The scene ignores definitions, and drops an instance that wasn't expanded.
    ui_scene s;
    scene_setup(&s);
    ui_cmd_writer *ops;
    ui_cmd_writer_create(&ops);
    ui_cmd_create_node(ops, ui_template_node(0), NUL);
    ui_cmd_define_template(w, 1, 1, ops);
    ui_cmd_destroy_template(w, 1);
    bytes = take(w, &size);
    CHECK(ui_scene_apply(&s, bytes, size, 0) == UI_OK);
    CHECK_EQ_INT(s.stats.dropped, 0);
    free(bytes);
    ui_cmd_instantiate_template(w, 1, SURFACE, NUL, (ui_handle[]){{9, 1}}, 1);
    bytes = take(w, &size);
    CHECK(ui_scene_apply(&s, bytes, size, 0) == UI_OK);
    CHECK_EQ_INT(s.stats.dropped, 1);
    free(bytes);
    ui_scene_destroy(&s);
    ui_cmd_writer_destroy(ops);
    ui_cmd_writer_destroy(w);
    ui_templates_destroy(t);
}

// tests/wire/fixtures/cmd-template-*.bin, for the older-reader check (docs/idl.md): written by
// `test_template --write-fixtures DIR`, then dumped with `test_wire_fixtures --write`.
static int write_fixture(const char *dir, const char *name, ui_cmd_writer *w) {
    size_t size;
    uint8_t *bytes = take(w, &size);
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(bytes, 1, size, f) == size;
    if (f) fclose(f);
    free(bytes);
    return ok ? 0 : 1;
}

static int write_fixtures(const char *dir) {
    ui_cmd_writer *w, *ops;
    ui_cmd_writer_create(&w);
    ui_cmd_writer_create(&ops);
    shape card = {.count = 4, .parent = {-1, 0, 0, -1}, .ops = {OP_STYLES | OP_STATE, OP_TEXT | OP_PROPS, OP_TOKENS | OP_TEXT, OP_TEXT}};
    ui_handle local[4] = {ui_template_node(0), ui_template_node(1), ui_template_node(2), ui_template_node(3)};
    write_shape(ops, &card, local, false, NUL, NUL);
    ui_cmd_define_template(w, 7, 4, ops);
    ui_cmd_instantiate_template(w, 7, SURFACE, NUL, (ui_handle[]){{1, 1}, {2, 1}, {3, 1}, {4, 1}}, 4);
    ui_cmd_instantiate_template(w, 7, SURFACE, (ui_handle){1, 1}, (ui_handle[]){{5, 1}, {6, 1}, {7, 1}, {8, 1}}, 4);
    ui_cmd_destroy_template(w, 7);
    int failed = write_fixture(dir, "cmd-template-card.bin", w);
    // Plain ops around templates: an older reader skips the template ops and reads the rest.
    ui_cmd_create_node(w, (ui_handle){1, 1}, SURFACE);
    ui_cmd_create_node(ops, ui_template_node(0), NUL);
    ui_cmd_set_state(ops, ui_template_node(0), UI_STATE_HOVER, UI_STATE_HOVER);
    ui_cmd_define_template(w, 2, 1, ops);
    ui_cmd_instantiate_template(w, 2, (ui_handle){1, 1}, NUL, (ui_handle[]){{2, 1}}, 1);
    ui_cmd_set_state(w, (ui_handle){2, 1}, UI_STATE_SELECTED, UI_STATE_SELECTED);
    ui_cmd_move_node(w, (ui_handle){2, 1}, SURFACE, (ui_handle){1, 1});
    failed |= write_fixture(dir, "cmd-template-mixed.bin", w);
    ui_cmd_writer_destroy(ops);
    ui_cmd_writer_destroy(w);
    return failed;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--write-fixtures")) return write_fixtures(argv[2]);
    RUN(test_equivalence_fixed);
    RUN(test_equivalence_random);
    RUN(test_rules);
    RUN(test_redefine_destroy);
    RUN(test_plain_buffers_untouched);
    return test_failures != 0;
}
