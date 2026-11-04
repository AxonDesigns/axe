// Token scopes (docs/styles.md, "Token scopes"): nested scopes, state blocks and per-node
// declarations, aliases resolved at the use site, cycles within a scope, kinds, moves,
// transitions, and how little a scope change computes again. Every flush also checks the
// scene against resolving everything from scratch (ui_scene_check_resolved).
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "core/scene.h"
#include "test.h"
#include "ui/ui.h"

static ui_logger quiet = {UI_LOG_ERROR};

typedef struct fixture {
    ui_scene s;
    ui_cmd_writer *w;
    ui_handle surface;
    uint64_t now;
} fixture;

static ui_handle h(uint32_t slot) { return (ui_handle){slot, 1}; }

static void flush(fixture *f) {
    uint8_t *bytes;
    size_t size;
    ui_cmd_writer_take(f->w, &bytes, &size);
    CHECK(ui_scene_apply(&f->s, bytes, size, f->now) == UI_OK);
    free(bytes);
    ui_scene_resolve(&f->s, f->now);
    CHECK(ui_scene_check_resolved(&f->s)); // as resolving every node from scratch
    ui_scene_animate(&f->s, f->now);
}

enum { BG = 1, USE = 1, FALLBACK = 2 }; // token 1; style 1 reads it, style 2 is the fallback

// 1 > 2 > {3, 4 > 5}, and 6 beside 1 at the surface. Every node lists FALLBACK (gray) and
// USE (background: token BG), and BG is red.
static void setup(fixture *f) {
    ui_scene_init(&f->s, &quiet);
    f->surface = (ui_handle){(uint32_t)UI_HANDLE_SURFACE << UI_HANDLE_SLOT_BITS, 1};
    ui_scene_add_root(&f->s, f->surface);
    ui_cmd_writer_create(&f->w);
    f->now = 0;
    ui_cmd_create_node(f->w, h(1), f->surface);
    ui_cmd_create_node(f->w, h(2), h(1));
    ui_cmd_create_node(f->w, h(3), h(2));
    ui_cmd_create_node(f->w, h(4), h(2));
    ui_cmd_create_node(f->w, h(5), h(4));
    ui_cmd_create_node(f->w, h(6), f->surface);
    ui_cmd_define_style(f->w, USE, &(ui_style_block_desc){.decls = &(ui_style_decl){UI_STYLE_BACKGROUND_COLOR, ui_token_ref(BG)}, .count = 1}, 1);
    ui_cmd_define_style(f->w, FALLBACK, &(ui_style_block_desc){.decls = &(ui_style_decl){UI_STYLE_BACKGROUND_COLOR, ui_rgba(0.5f, 0.5f, 0.5f, 1)}, .count = 1}, 1);
    for (uint32_t i = 1; i <= 6; i++) ui_cmd_set_styles(f->w, h(i), (uint32_t[]){FALLBACK, USE}, 2);
    ui_cmd_set_token(f->w, BG, ui_rgba(1, 0, 0, 1));
}

static void teardown(fixture *f) {
    CHECK_EQ_INT(f->s.stats.dropped, 0);
    ui_cmd_writer_destroy(f->w);
    ui_scene_destroy(&f->s);
}

static const float *bg_of(fixture *f, uint32_t slot) { return f->s.nodes[slot].computed.background; }

static bool is(const float *c, float r, float g, float b) {
    return fabsf(c[0] - r) < 1e-5f && fabsf(c[1] - g) < 1e-5f && fabsf(c[2] - b) < 1e-5f;
}
static bool red(fixture *f, uint32_t slot) { return is(bg_of(f, slot), 1, 0, 0); }
static bool green(fixture *f, uint32_t slot) { return is(bg_of(f, slot), 0, 1, 0); }
static bool blue(fixture *f, uint32_t slot) { return is(bg_of(f, slot), 0, 0, 1); }
static bool white(fixture *f, uint32_t slot) { return is(bg_of(f, slot), 1, 1, 1); }
static bool gray(fixture *f, uint32_t slot) { return bg_of(f, slot)[0] > 0.2f && bg_of(f, slot)[0] < 0.3f; }

static ui_token_decl decl(uint32_t token, ui_value v) { return (ui_token_decl){token, v}; }

// A style whose base block declares one token.
static void scope_style(fixture *f, uint32_t id, ui_token_decl d) {
    CHECK(ui_cmd_define_style(f->w, id, &(ui_style_block_desc){.tokens = &d, .token_count = 1}, 1) == UI_OK);
}

static void styles(fixture *f, uint32_t slot, uint32_t extra) {
    uint32_t ids[3] = {FALLBACK, USE, extra};
    ui_cmd_set_styles(f->w, h(slot), ids, extra ? 3 : 2);
}

static void own(fixture *f, uint32_t slot, ui_token_decl d) {
    CHECK(ui_cmd_set_token_overrides(f->w, h(slot), &d, 1) == UI_OK);
}

static uint64_t computed(fixture *f) { return f->s.stats.styles_computed; }

// The nearest declaration wins, its own or an ancestor's; global values elsewhere. A change
// computes again only the nodes reading the token as that scope gives it, and stops at a
// descendant declaring the token itself.
static void test_nested(void) {
    fixture f;
    setup(&f);
    scope_style(&f, 10, decl(BG, ui_rgba(0, 1, 0, 1)));
    styles(&f, 2, 10);
    own(&f, 4, decl(BG, ui_rgba(0, 0, 1, 1)));
    flush(&f);
    CHECK(red(&f, 1) && green(&f, 2) && green(&f, 3) && blue(&f, 4) && blue(&f, 5) && red(&f, 6));
    // The scope changes its value: 2 and 3 compute again, not 4 and 5.
    scope_style(&f, 10, decl(BG, ui_rgba(1, 1, 1, 1)));
    uint64_t before = computed(&f);
    flush(&f);
    CHECK(white(&f, 2) && white(&f, 3) && blue(&f, 4) && blue(&f, 5) && red(&f, 1));
    CHECK_EQ_INT(computed(&f) - before, 2);
    // Another list for 2 that declares the same: 2 computes (its cascade), nothing else.
    scope_style(&f, 11, decl(BG, ui_rgba(1, 1, 1, 1)));
    styles(&f, 2, 11);
    before = computed(&f);
    flush(&f);
    CHECK_EQ_INT(computed(&f) - before, 1);
    // The global value reaches what no scope covers.
    ui_cmd_set_token(f.w, BG, ui_rgba(0, 1, 0, 1));
    flush(&f);
    CHECK(green(&f, 1) && green(&f, 6) && white(&f, 3) && blue(&f, 5));
    // Removing 4's own declaration: 4 and 5 read 2's.
    own(&f, 4, decl(BG, ui_unset()));
    flush(&f);
    CHECK(white(&f, 4) && white(&f, 5));
    // No scope left: global everywhere.
    styles(&f, 2, 0);
    flush(&f);
    for (uint32_t i = 1; i <= 6; i++) CHECK(green(&f, i));
    teardown(&f);
}

// State blocks declare tokens too, and win over the base block as their entries do; the
// node's own declarations win over every block.
static void test_states(void) {
    fixture f;
    setup(&f);
    ui_token_decl base = decl(BG, ui_rgba(0, 1, 0, 1)), hover = decl(BG, ui_rgba(0, 0, 1, 1));
    ui_style_block_desc blocks[] = {{.tokens = &base, .token_count = 1},
                                    {.states = UI_STATE_HOVER, .tokens = &hover, .token_count = 1}};
    CHECK(ui_cmd_define_style(f.w, 10, blocks, 2) == UI_OK);
    styles(&f, 2, 10);
    flush(&f);
    CHECK(green(&f, 2) && green(&f, 5));
    ui_cmd_set_state(f.w, h(2), UI_STATE_HOVER, UI_STATE_HOVER);
    flush(&f);
    CHECK(blue(&f, 2) && blue(&f, 3) && blue(&f, 5) && red(&f, 1));
    own(&f, 2, decl(BG, ui_rgba(1, 1, 1, 1)));
    flush(&f);
    CHECK(white(&f, 2) && white(&f, 5));
    own(&f, 2, decl(BG, ui_unset()));
    ui_cmd_set_state(f.w, h(2), UI_STATE_HOVER, 0);
    flush(&f);
    CHECK(green(&f, 2) && green(&f, 5));
    teardown(&f);
}

enum { PALETTE = 20, ACCENT = 21, OTHER = 22 };

// Aliases resolve where they are used: each link of a chain is looked up in the scope of the
// node reading it, so a palette token declared in a subtree changes the aliases of it there.
static void test_use_site_aliases(void) {
    fixture f;
    setup(&f);
    ui_cmd_set_token(f.w, PALETTE, ui_rgba(1, 0, 0, 1));
    ui_cmd_set_token(f.w, ACCENT, ui_token_ref(PALETTE));
    ui_cmd_set_token(f.w, BG, ui_token_ref(ACCENT)); // BG -> ACCENT -> PALETTE
    own(&f, 2, decl(PALETTE, ui_rgba(0, 1, 0, 1)));
    flush(&f);
    CHECK(red(&f, 1) && green(&f, 2) && green(&f, 3) && green(&f, 5) && red(&f, 6));
    // A declaration that is itself an alias, looked up at the node: 4 says ACCENT is OTHER.
    ui_cmd_set_token(f.w, OTHER, ui_rgba(0, 0, 1, 1));
    own(&f, 4, decl(ACCENT, ui_token_ref(OTHER)));
    flush(&f);
    CHECK(blue(&f, 4) && blue(&f, 5) && green(&f, 3));
    // A global change at the end of a scoped chain reaches the nodes reading through it.
    ui_cmd_set_token(f.w, OTHER, ui_rgba(1, 1, 1, 1));
    flush(&f);
    CHECK(white(&f, 4) && white(&f, 5) && green(&f, 3) && red(&f, 1));
    // So does one in the middle of it: ACCENT's global value no longer matters under 4.
    ui_cmd_set_token(f.w, ACCENT, ui_rgba(0, 0, 1, 1));
    flush(&f);
    CHECK(blue(&f, 1) && blue(&f, 3) && white(&f, 5));
    teardown(&f);
}

// A cycle in the chain as a node sees it makes the declaration absent there only; a token in
// a global cycle is invalid everywhere, declared in a scope or not.
static void test_cycles(void) {
    fixture f;
    setup(&f);
    ui_cmd_set_token(f.w, PALETTE, ui_rgba(1, 0, 0, 1));
    ui_cmd_set_token(f.w, ACCENT, ui_token_ref(PALETTE));
    ui_cmd_set_token(f.w, BG, ui_token_ref(ACCENT));
    own(&f, 2, decl(PALETTE, ui_token_ref(BG))); // BG -> ACCENT -> PALETTE -> BG, under 2
    flush(&f);
    CHECK(red(&f, 1) && gray(&f, 2) && gray(&f, 5) && red(&f, 6));
    // Breaking it in a nested scope.
    own(&f, 4, decl(ACCENT, ui_rgba(0, 0, 1, 1)));
    flush(&f);
    CHECK(gray(&f, 3) && blue(&f, 4) && blue(&f, 5));
    // A global cycle: ACCENT <-> OTHER. Declaring ACCENT in a scope doesn't rescue it.
    ui_cmd_set_token(f.w, ACCENT, ui_token_ref(OTHER));
    ui_cmd_set_token(f.w, OTHER, ui_token_ref(ACCENT));
    flush(&f);
    CHECK(gray(&f, 1) && gray(&f, 4) && gray(&f, 5));
    ui_cmd_set_token(f.w, OTHER, ui_rgba(0, 1, 0, 1));
    flush(&f);
    CHECK(green(&f, 1) && blue(&f, 4) && blue(&f, 5));
    teardown(&f);
}

enum { LOCAL = 30, SIZE = 31 };

// A declaration must have its token's kind (its global value's, through global aliases); one
// that doesn't is ignored, and the scope around it, or the global value, shows. A local token
// (no global value) takes any.
static void test_kinds(void) {
    fixture f;
    setup(&f);
    own(&f, 2, decl(BG, ui_rgba(0, 1, 0, 1)));
    own(&f, 4, decl(BG, ui_px(3))); // BG is a color: ignored, 2's shows
    flush(&f);
    CHECK(green(&f, 4) && green(&f, 5));
    // An alias to a token of another kind is ignored too; to a local token, taken.
    ui_cmd_set_token(f.w, SIZE, ui_px(4));
    own(&f, 4, decl(BG, ui_token_ref(SIZE)));
    flush(&f);
    CHECK(green(&f, 4) && green(&f, 5));
    own(&f, 4, decl(BG, ui_token_ref(LOCAL)));
    own(&f, 3, decl(LOCAL, ui_rgba(0, 0, 1, 1)));
    own(&f, 2, decl(LOCAL, ui_rgba(1, 1, 1, 1)));
    flush(&f);
    CHECK(white(&f, 4) && white(&f, 5) && green(&f, 3)); // 3 declares LOCAL but reads BG
    // A local token read with no declaration in reach is missing: the fallback shows.
    ui_cmd_set_styles(f.w, h(6), (uint32_t[]){FALLBACK, 40}, 2);
    ui_cmd_define_style(f.w, 40, &(ui_style_block_desc){.decls = &(ui_style_decl){UI_STYLE_BACKGROUND_COLOR, ui_token_ref(LOCAL)}, .count = 1}, 1);
    flush(&f);
    CHECK(gray(&f, 6));
    // Its kind follows its global value: SIZE becomes a color, and 4's alias is taken.
    ui_cmd_set_token(f.w, SIZE, ui_rgba(0, 0, 1, 1));
    own(&f, 4, decl(BG, ui_token_ref(SIZE)));
    flush(&f);
    CHECK(blue(&f, 4) && blue(&f, 5));
    // BG becomes a length: 2's color is ignored now, and 4's alias with it; BG's px can't
    // be a background, so the fallback shows.
    ui_cmd_set_token(f.w, BG, ui_px(2));
    flush(&f);
    CHECK(gray(&f, 2) && gray(&f, 5) && gray(&f, 1));
    ui_cmd_set_token(f.w, BG, ui_rgba(1, 0, 0, 1));
    flush(&f);
    CHECK(green(&f, 2) && blue(&f, 5) && red(&f, 1));
    // Unsetting makes a token local: every declaration is taken again.
    own(&f, 2, decl(BG, ui_px(1)));
    flush(&f);
    CHECK(red(&f, 2) && blue(&f, 5));
    ui_cmd_set_token(f.w, BG, ui_unset());
    flush(&f);
    CHECK(gray(&f, 2) && blue(&f, 5) && gray(&f, 1)); // a px for a background: absent
    teardown(&f);
}

// Moving a node into another scope changes what it and its subtree read; one with a scope of
// its own takes it along.
static void test_moves(void) {
    fixture f;
    setup(&f);
    own(&f, 2, decl(BG, ui_rgba(0, 1, 0, 1)));
    own(&f, 4, decl(PALETTE, ui_rgba(0, 0, 1, 1))); // a scope that doesn't declare BG
    ui_cmd_create_node(f.w, h(7), h(6));
    ui_cmd_create_node(f.w, h(8), h(7));
    styles(&f, 7, 0);
    styles(&f, 8, 0);
    flush(&f);
    CHECK(green(&f, 5) && red(&f, 8));
    ui_cmd_move_node(f.w, h(7), h(3), (ui_handle){0}); // into 2's scope, with 8
    flush(&f);
    CHECK(green(&f, 7) && green(&f, 8));
    ui_cmd_move_node(f.w, h(4), f.surface, (ui_handle){0}); // out of it, with its own scope
    flush(&f);
    CHECK(red(&f, 4) && red(&f, 5));
    ui_cmd_move_node(f.w, h(4), h(8), (ui_handle){0}); // back in, deeper
    flush(&f);
    CHECK(green(&f, 4) && green(&f, 5));
    // A node moved out before its scope's owner is destroyed keeps nothing of it.
    ui_cmd_move_node(f.w, h(5), h(6), (ui_handle){0});
    ui_cmd_destroy_node(f.w, h(2));
    flush(&f);
    CHECK(red(&f, 5) && f.s.nodes[5].scope == NULL);
    teardown(&f);
}

// Values that change because a scope changed animate through their transitions.
static void test_transitions(void) {
    fixture f;
    setup(&f);
    ui_cmd_set_props(f.w, h(3), &(ui_style_decl){UI_STYLE_TRANSITION(UI_STYLE_BACKGROUND_COLOR), ui_transition(200, 20)}, 1);
    flush(&f);
    own(&f, 2, decl(BG, ui_rgba(0, 0, 1, 1)));
    f.now = 1000000;
    flush(&f);
    const ui_node *n = &f.s.nodes[3];
    CHECK(n->anim && n->anim->springs[UI_SPRING_COLOR].active);
    CHECK(n->anim && n->anim->springs[UI_SPRING_COLOR].target[2] == 1.0);
    CHECK(n->color[0] > 0.5f); // still mostly red
    CHECK(f.s.nodes[2].color[2] == 1); // no transition: it jumps
    teardown(&f);
}

// Text: a scoped token reaches inherited values, which the subtree inherits.
static void test_text(void) {
    fixture f;
    setup(&f);
    ui_cmd_set_token(f.w, PALETTE, ui_rgba(1, 0, 0, 1));
    ui_cmd_set_props(f.w, h(2), &(ui_style_decl){UI_STYLE_TEXT_COLOR, ui_token_ref(PALETTE)}, 1);
    ui_cmd_set_props(f.w, h(4), &(ui_style_decl){UI_STYLE_FONT_FAMILY, ui_token_ref(ACCENT)}, 1);
    ui_cmd_set_token(f.w, ACCENT, ui_string("Noto Sans"));
    own(&f, 2, decl(PALETTE, ui_rgba(0, 1, 0, 1)));
    own(&f, 4, decl(ACCENT, ui_string("Noto Serif")));
    flush(&f);
    CHECK(is(f.s.nodes[5].computed.text.color, 0, 1, 0));
    CHECK(f.s.nodes[5].computed.text.family && !strcmp(f.s.nodes[5].computed.text.family, "Noto Serif"));
    own(&f, 4, decl(ACCENT, ui_px(3))); // ACCENT is a string: ignored
    flush(&f);
    CHECK(f.s.nodes[5].computed.text.family && !strcmp(f.s.nodes[5].computed.text.family, "Noto Sans"));
    teardown(&f);
}

// Many readers under a scope: switched by a style list (one walk of the tree) and by a state
// (a short queue, the subtree resolved in order), with nested scopes inside.
static void test_large(void) {
    fixture f;
    setup(&f);
    for (uint32_t i = 0; i < 400; i++) {
        ui_cmd_create_node(f.w, h(10 + i), h(i < 10 ? 3 : 10 + (i - 10) / 3));
        ui_cmd_set_styles(f.w, h(10 + i), (uint32_t[]){FALLBACK, USE}, 2);
    }
    own(&f, 100, decl(BG, ui_rgba(1, 1, 1, 1))); // a nested scope hiding its subtree (290 is in it)
    ui_token_decl hover = decl(BG, ui_rgba(0, 0, 1, 1));
    ui_style_block_desc blocks[] = {{.tokens = &(ui_token_decl){BG, ui_rgba(0, 1, 0, 1)}, .token_count = 1},
                                    {.states = UI_STATE_HOVER, .tokens = &hover, .token_count = 1}};
    CHECK(ui_cmd_define_style(f.w, 10, blocks, 2) == UI_OK);
    flush(&f);
    CHECK(red(&f, 409) && white(&f, 100) && white(&f, 290));
    styles(&f, 2, 10);
    flush(&f);
    CHECK(green(&f, 3) && green(&f, 409) && white(&f, 290));
    ui_cmd_set_state(f.w, h(2), UI_STATE_HOVER, UI_STATE_HOVER);
    uint64_t before = computed(&f);
    flush(&f);
    CHECK(blue(&f, 409) && white(&f, 100) && white(&f, 290));
    uint32_t hidden = 0; // 100 and its subtree
    for (uint32_t i = 10; i < 410; i++) {
        uint32_t up = i;
        while (up != 100 && up >= 10) up = f.s.nodes[up].parent;
        hidden += up == 100;
    }
    CHECK_EQ_INT(computed(&f) - before, 1 + 3 + 400 - hidden); // 2; 3, 4, 5; the readers 100 doesn't hide
    teardown(&f);
}

int main(void) {
    RUN(test_nested);
    RUN(test_states);
    RUN(test_use_site_aliases);
    RUN(test_cycles);
    RUN(test_kinds);
    RUN(test_moves);
    RUN(test_transitions);
    RUN(test_text);
    RUN(test_large);
    return test_failures ? 1 : 0;
}
