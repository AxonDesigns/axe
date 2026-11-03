// Styles on the scene (docs/styles.md): precedence, state variants by specificity, tokens
// (chains, cycles, wrong types), transitions started each way, set_* ops as overrides,
// redefinition and destruction, text properties, inheritance (inherit and initial, minimal
// recomputation, set_text's values), interned strings.
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
    ui_handle surface, node;
    uint64_t now;
} fixture;

static void setup(fixture *f) {
    ui_scene_init(&f->s, &quiet);
    f->surface = (ui_handle){(uint32_t)UI_HANDLE_SURFACE << UI_HANDLE_SLOT_BITS, 1};
    ui_scene_add_root(&f->s, f->surface);
    ui_cmd_writer_create(&f->w);
    f->node = (ui_handle){1, 1};
    f->now = 0;
    ui_cmd_create_node(f->w, f->node, f->surface);
}

// Applies what was written, then resolves and animates at the fixture's time.
static void flush(fixture *f) {
    uint8_t *bytes;
    size_t size;
    ui_cmd_writer_take(f->w, &bytes, &size);
    CHECK(ui_scene_apply(&f->s, bytes, size, f->now) == UI_OK);
    free(bytes);
    ui_scene_resolve(&f->s, f->now);
    ui_scene_animate(&f->s, f->now);
}

static void teardown(fixture *f) {
    CHECK_EQ_INT(f->s.stats.dropped, 0);
    ui_cmd_writer_destroy(f->w);
    ui_scene_destroy(&f->s);
}

static ui_node *n_of(fixture *f) { return &f->s.nodes[ui_handle_slot(f->node)]; }

// Defines a style with a base block and optional state blocks of one background color each.
static void define(fixture *f, uint32_t id, const ui_style_block_desc *blocks, uint32_t n) {
    CHECK(ui_cmd_define_style(f->w, id, blocks, n) == UI_OK);
}

static ui_style_decl bg(float r, float g, float b) {
    return (ui_style_decl){UI_STYLE_BACKGROUND_COLOR, ui_rgba(r, g, b, 1)};
}

// The node's drawn background in sRGB-ish terms: 1 means the channel is at full.
static bool is_rgb(fixture *f, float r, float g, float b) {
    const float *c = n_of(f)->color;
    return fabsf(c[0] - r) < 1e-4f && fabsf(c[1] - g) < 1e-4f && fabsf(c[2] - b) < 1e-4f;
}

static void test_precedence(void) {
    fixture f;
    setup(&f);
    ui_style_decl red = bg(1, 0, 0), green = bg(0, 1, 0), blue = bg(0, 0, 1);
    define(&f, 1, &(ui_style_block_desc){0, &red, 1}, 1);
    define(&f, 2, &(ui_style_block_desc){0, &green, 1}, 1);
    ui_cmd_set_styles(f.w, f.node, (uint32_t[]){1, 2}, 2);
    flush(&f);
    CHECK(is_rgb(&f, 0, 1, 0)); // later styles win
    ui_cmd_set_styles(f.w, f.node, (uint32_t[]){2, 1}, 2);
    flush(&f);
    CHECK(is_rgb(&f, 1, 0, 0));
    ui_cmd_set_props(f.w, f.node, &blue, 1); // overrides win over every style
    flush(&f);
    CHECK(is_rgb(&f, 0, 0, 1));
    ui_cmd_set_props(f.w, f.node, &(ui_style_decl){UI_STYLE_BACKGROUND_COLOR, ui_unset()}, 1);
    flush(&f);
    CHECK(is_rgb(&f, 1, 0, 0));
    // An undefined style contributes nothing until it is defined.
    ui_cmd_set_styles(f.w, f.node, (uint32_t[]){1, 3}, 2);
    flush(&f);
    CHECK(is_rgb(&f, 1, 0, 0));
    define(&f, 3, &(ui_style_block_desc){0, &blue, 1}, 1);
    flush(&f);
    CHECK(is_rgb(&f, 0, 0, 1));
    // Redefining updates users; destroying removes it from their resolution.
    ui_style_decl gray = bg(0.5f, 0.5f, 0.5f);
    define(&f, 3, &(ui_style_block_desc){0, &gray, 1}, 1);
    flush(&f);
    CHECK(is_rgb(&f, 0.5f, 0.5f, 0.5f) || n_of(&f)->color[0] < 0.25f); // linear 0.214
    CHECK_NEAR(n_of(&f)->color[0], 0.2140f, 1e-3);
    CHECK(ui_cmd_destroy_style(f.w, 3) == UI_OK);
    flush(&f);
    CHECK(is_rgb(&f, 1, 0, 0));
    teardown(&f);
}

static void test_state_specificity(void) {
    fixture f;
    setup(&f);
    ui_style_decl base_a = bg(1, 1, 1), hover_a = bg(1, 0, 0), both_a = bg(0, 1, 0);
    ui_style_decl base_b = bg(0, 0, 0), hover_b = bg(0, 0, 1);
    ui_style_block_desc a[] = {{0, &base_a, 1},
                               {UI_STATE_HOVER, &hover_a, 1},
                               {UI_STATE_HOVER | UI_STATE_PRESSED, &both_a, 1}};
    ui_style_block_desc b[] = {{0, &base_b, 1}, {UI_STATE_HOVER, &hover_b, 1}};
    define(&f, 1, a, 3);
    define(&f, 2, b, 2);
    ui_cmd_set_styles(f.w, f.node, (uint32_t[]){1, 2}, 2);
    flush(&f);
    CHECK(is_rgb(&f, 0, 0, 0)); // bases in list order
    ui_cmd_set_state(f.w, f.node, UI_STATE_HOVER, UI_STATE_HOVER);
    flush(&f);
    CHECK(is_rgb(&f, 0, 0, 1)); // both hovers match: the later style's wins
    ui_cmd_set_state(f.w, f.node, UI_STATE_PRESSED, UI_STATE_PRESSED);
    flush(&f);
    CHECK(is_rgb(&f, 0, 1, 0)); // hover+pressed is more specific than a later hover
    // An earlier style's hover beats a later style's base.
    ui_cmd_set_styles(f.w, f.node, (uint32_t[]){1, 3}, 2);
    ui_style_decl base_c = bg(0.5f, 0, 0);
    define(&f, 3, &(ui_style_block_desc){0, &base_c, 1}, 1);
    ui_cmd_set_state(f.w, f.node, UI_STATE_PRESSED, 0);
    flush(&f);
    CHECK(is_rgb(&f, 1, 0, 0));
    ui_cmd_set_state(f.w, f.node, 0xFFu, 0);
    flush(&f);
    CHECK_NEAR(n_of(&f)->color[0], 0.2140f, 1e-3); // back to the later base
    teardown(&f);
}

static void test_tokens(void) {
    fixture f;
    setup(&f);
    // Token 1 -> token 2 -> a color; token 3 <-> token 4 loop; token 5 is a length.
    ui_cmd_set_token(f.w, 2, ui_rgba(0, 1, 0, 1));
    ui_cmd_set_token(f.w, 1, ui_token_ref(2));
    ui_cmd_set_token(f.w, 3, ui_token_ref(4));
    ui_cmd_set_token(f.w, 4, ui_token_ref(3));
    ui_cmd_set_token(f.w, 5, ui_px(4));
    ui_style_decl fallback = bg(1, 0, 0), chained = {UI_STYLE_BACKGROUND_COLOR, ui_token_ref(1)};
    define(&f, 1, &(ui_style_block_desc){0, &fallback, 1}, 1);
    define(&f, 2, &(ui_style_block_desc){0, &chained, 1}, 1);
    ui_cmd_set_styles(f.w, f.node, (uint32_t[]){1, 2}, 2);
    flush(&f);
    CHECK(is_rgb(&f, 0, 1, 0)); // through the chain
    // Changing the end of the chain updates every user.
    ui_cmd_set_token(f.w, 2, ui_rgba(0, 0, 1, 1));
    flush(&f);
    CHECK(is_rgb(&f, 0, 0, 1));
    // A cycle, a wrong type and a missing token each count as absent: the lower value shows.
    const uint32_t refs[] = {3, 5, 99};
    for (int i = 0; i < 3; i++) {
        ui_style_decl d = {UI_STYLE_BACKGROUND_COLOR, ui_token_ref(refs[i])};
        define(&f, 2, &(ui_style_block_desc){0, &d, 1}, 1);
        flush(&f);
        CHECK(is_rgb(&f, 1, 0, 0));
    }
    // Defining the missing token later makes it apply; removing it falls back again.
    ui_cmd_set_token(f.w, 99, ui_rgba(0, 1, 1, 1));
    flush(&f);
    CHECK(is_rgb(&f, 0, 1, 1));
    ui_cmd_set_token(f.w, 99, ui_unset());
    flush(&f);
    CHECK(is_rgb(&f, 1, 0, 0));
    // A token in an override.
    ui_cmd_set_props(f.w, f.node, &(ui_style_decl){UI_STYLE_BACKGROUND_COLOR, ui_token_ref(1)}, 1);
    flush(&f);
    CHECK(is_rgb(&f, 0, 0, 1));
    ui_cmd_set_token(f.w, 2, ui_rgba(1, 1, 0, 1));
    flush(&f);
    CHECK(is_rgb(&f, 1, 1, 0));
    // The writer refuses values a token can't hold.
    CHECK(ui_cmd_set_token(f.w, 6, ui_transition(100, 10)) == UI_ERR_INVALID_ARGUMENT);
    CHECK(ui_cmd_set_token(f.w, 0, ui_px(1)) == UI_ERR_INVALID_ARGUMENT);
    teardown(&f);
}

// Each kind of change starts the declared transition; values then move over time.
static void test_transitions(void) {
    fixture f;
    setup(&f);
    ui_style_decl anim[] = {bg(1, 0, 0), {UI_STYLE_TRANSITION(UI_STYLE_BACKGROUND_COLOR), ui_transition(200, 28)}};
    ui_style_decl hover = bg(0, 0, 1), other = bg(0, 1, 0);
    ui_style_block_desc blocks[] = {{0, anim, 2}, {UI_STATE_HOVER, &hover, 1}};
    define(&f, 1, blocks, 2);
    define(&f, 2, &(ui_style_block_desc){0, &other, 1}, 1);
    ui_cmd_set_styles(f.w, f.node, (uint32_t[]){1}, 1);
    flush(&f);
    CHECK(is_rgb(&f, 1, 0, 0)); // the first resolution never animates
    CHECK(!n_of(&f)->anim || !n_of(&f)->anim->springs[UI_SPRING_COLOR].active);

    ui_cmd_set_token(f.w, 1, ui_rgba(1, 1, 1, 1));
    flush(&f);
    // 1. A state change.
    ui_cmd_set_state(f.w, f.node, UI_STATE_HOVER, UI_STATE_HOVER);
    flush(&f);
    ui_node *n = n_of(&f);
    CHECK(n->anim && n->anim->springs[UI_SPRING_COLOR].active);
    CHECK(is_rgb(&f, 1, 0, 0)); // still where it was at the start
    f.now += 50000000;
    ui_scene_animate(&f.s, f.now);
    CHECK(n->color[0] < 1 && n->color[2] > 0); // on its way to blue
    // 2. An override, retargeting mid-flight: velocity is kept, so there is no jump.
    float before[4];
    memcpy(before, n->color, sizeof before);
    ui_cmd_set_props(f.w, f.node, &(ui_style_decl){UI_STYLE_BACKGROUND_COLOR, ui_rgba(0, 1, 0, 1)}, 1);
    flush(&f);
    for (int i = 0; i < 4; i++) CHECK_NEAR(n->color[i], before[i], 1e-6);
    CHECK_NEAR(n->anim->springs[UI_SPRING_COLOR].target[1], 1, 1e-6);
    f.now += 3000000000ull;
    ui_scene_animate(&f.s, f.now);
    CHECK(is_rgb(&f, 0, 1, 0) && !n->anim->springs[UI_SPRING_COLOR].active);
    // 3. A token the style uses.
    ui_cmd_set_props(f.w, f.node, &(ui_style_decl){UI_STYLE_BACKGROUND_COLOR, ui_unset()}, 1);
    ui_cmd_set_state(f.w, f.node, UI_STATE_HOVER, 0);
    ui_style_decl tokened[] = {{UI_STYLE_BACKGROUND_COLOR, ui_token_ref(1)},
                               {UI_STYLE_TRANSITION(UI_STYLE_BACKGROUND_COLOR), ui_transition(200, 28)}};
    define(&f, 1, &(ui_style_block_desc){0, tokened, 2}, 1);
    flush(&f);
    f.now += 3000000000ull;
    ui_scene_animate(&f.s, f.now);
    CHECK(is_rgb(&f, 1, 1, 1));
    ui_cmd_set_token(f.w, 1, ui_rgba(0, 0, 0, 1));
    flush(&f);
    CHECK(n->anim->springs[UI_SPRING_COLOR].active && is_rgb(&f, 1, 1, 1));
    f.now += 3000000000ull;
    ui_scene_animate(&f.s, f.now);
    CHECK(is_rgb(&f, 0, 0, 0));
    // 4. The style list: style 2 (green, no transition of its own) on top of style 1, whose
    // transition still applies to the background.
    ui_cmd_set_styles(f.w, f.node, (uint32_t[]){1, 2}, 2);
    flush(&f);
    CHECK(n->anim->springs[UI_SPRING_COLOR].active);
    f.now += 3000000000ull;
    ui_scene_animate(&f.s, f.now);
    CHECK(is_rgb(&f, 0, 1, 0));
    teardown(&f);
}

// set_color and set_corner_radius are overrides: they jump (stopping a spring), unless the
// styles declare a transition; start_spring still animates with its own spring.
static void test_legacy_ops(void) {
    fixture f;
    setup(&f);
    ui_cmd_set_color(f.w, f.node, 1, 0, 0, 1);
    ui_cmd_set_corner_radius(f.w, f.node, 4);
    flush(&f);
    ui_node *n = n_of(&f);
    CHECK(is_rgb(&f, 1, 0, 0) && n->radius == 4);
    float target[4] = {0, 0, 1, 1};
    ui_cmd_start_spring(f.w, f.node, UI_PROP_COLOR, target, 100, 20);
    flush(&f);
    CHECK(n->anim && n->anim->springs[UI_SPRING_COLOR].active);
    CHECK_NEAR(n->anim->springs[UI_SPRING_COLOR].stiffness, 100, 1e-9);
    f.now += 30000000;
    ui_scene_animate(&f.s, f.now);
    CHECK(n->color[2] > 0 && n->color[0] < 1);
    ui_cmd_set_color(f.w, f.node, 0, 1, 0, 1); // jumps, as in milestone 0
    flush(&f);
    CHECK(is_rgb(&f, 0, 1, 0) && !n->anim->springs[UI_SPRING_COLOR].active);
    // With a declared transition, set_color animates instead.
    ui_style_decl t = {UI_STYLE_TRANSITION(UI_STYLE_BACKGROUND_COLOR), ui_transition(300, 30)};
    define(&f, 1, &(ui_style_block_desc){0, &t, 1}, 1);
    ui_cmd_set_styles(f.w, f.node, (uint32_t[]){1}, 1);
    flush(&f);
    ui_cmd_set_color(f.w, f.node, 1, 1, 0, 1);
    flush(&f);
    CHECK(n->anim->springs[UI_SPRING_COLOR].active && is_rgb(&f, 0, 1, 0));
    // set_clip is an override too.
    ui_cmd_set_clip(f.w, f.node, true);
    flush(&f);
    CHECK(n->clip && n->computed.clip);
    teardown(&f);
}

static void test_paint_values(void) {
    fixture f;
    setup(&f);
    ui_style_decl d[] = {{UI_STYLE_BORDER_WIDTH, ui_px(2)},
                         {UI_STYLE_BORDER_COLOR, ui_rgba(0, 0, 0, 0.5f)},
                         {UI_STYLE_CORNER_RADIUS, ui_px(6)},
                         {UI_STYLE_OPACITY, ui_number(0.25f)},
                         {UI_STYLE_CLIP, ui_keyword(UI_CLIP_RECT)}};
    define(&f, 1, &(ui_style_block_desc){0, d, 5}, 1);
    ui_cmd_set_styles(f.w, f.node, (uint32_t[]){1}, 1);
    flush(&f);
    ui_node *n = n_of(&f);
    CHECK(n->border_width == 2 && n->border_color[3] == 0.5f && n->radius == 6);
    CHECK(n->opacity == 0.25f && n->clip);
    // The writer refuses what a property can't take.
    ui_style_decl bad[] = {{UI_STYLE_OPACITY, ui_number(2)},
                           {UI_STYLE_BORDER_WIDTH, ui_px(-1)},
                           {UI_STYLE_BACKGROUND_COLOR, ui_px(1)},
                           {UI_STYLE_TRANSITION(UI_STYLE_BORDER_WIDTH), ui_transition(1, 1)},
                           {999, ui_px(1)}};
    for (int i = 0; i < 5; i++)
        CHECK(ui_cmd_define_style(f.w, 9, &(ui_style_block_desc){0, &bad[i], 1}, 1) ==
              UI_ERR_INVALID_ARGUMENT);
    CHECK(ui_cmd_define_style(f.w, 9, &(ui_style_block_desc){0, &(ui_style_decl){UI_STYLE_OPACITY, ui_unset()}, 1}, 1) ==
          UI_ERR_INVALID_ARGUMENT); // unset is for overrides only
    CHECK(ui_cmd_set_styles(f.w, f.node, (uint32_t[]){0}, 1) == UI_ERR_INVALID_ARGUMENT);
    teardown(&f);
}

// Text: styles override set_text's paragraph values; spans keep their colors.
static void test_text_values(void) {
    fixture f;
    setup(&f);
    static const char *const fam[] = {"Noto Sans"};
    ui_text_span_desc span = {0, 2, {.family = 0, .weight = 700, .size = 14, .color = {1, 0, 0, 1}}, UI_TEXT_SET_SPAN_ALL};
    ui_text_desc t = {.set = UI_TEXT_SET_ALL, .text = "Hi there", .length = 8,
                      .style = {.family = 0, .weight = 400, .size = 14, .color = {0, 0, 1, 1}},
                      .spans = &span, .span_count = 1, .families = fam, .family_count = 1,
                      .align = UI_TEXT_ALIGN_START, .line_height = 1.2f, .max_lines = 3};
    ui_cmd_set_text(f.w, f.node, &t);
    flush(&f);
    ui_node *n = n_of(&f);
    CHECK(is_rgb(&f, 0, 0, 0)); // no background
    CHECK(n->text_color[2] == 1 && n->text_color[0] == 0); // set_text's color
    CHECK(n->text->align == UI_ALIGN_START && n->text->max_lines == 3);
    ui_style_decl d[] = {{UI_STYLE_TEXT_COLOR, ui_rgba(0, 1, 0, 1)},
                         {UI_STYLE_TEXT_ALIGN, ui_keyword(UI_TEXT_ALIGN_CENTER)},
                         {UI_STYLE_LINE_HEIGHT, ui_auto()},
                         {UI_STYLE_MAX_LINES, ui_none()}};
    define(&f, 1, &(ui_style_block_desc){0, d, 4}, 1);
    ui_cmd_set_styles(f.w, f.node, (uint32_t[]){1}, 1);
    flush(&f);
    CHECK(n->text_color[1] == 1 && n->text_color[2] == 0);
    CHECK(n->text->align == UI_ALIGN_CENTER && n->text->max_lines == 0 && n->text->line_height == 0);
    CHECK(n->text->spans[0].style.color[0] == 1); // the span keeps red
    // New text keeps the styles' values.
    t.style.color[1] = 0.5f;
    ui_cmd_set_text(f.w, f.node, &t);
    flush(&f);
    CHECK(n->text->align == UI_ALIGN_CENTER && n->text_color[1] == 1);
    // Without the style, set_text's values come back.
    ui_cmd_set_styles(f.w, f.node, NULL, 0);
    flush(&f);
    CHECK(n->text->align == UI_ALIGN_START && n->text->max_lines == 3);
    CHECK_NEAR(n->text->line_height, 1.2, 1e-6);
    teardown(&f);
}

// ---- Inheritance -------------------------------------------------------------------------

static ui_handle h(uint32_t slot) { return (ui_handle){slot, 1}; }
static const ui_computed *cv(fixture *f, uint32_t slot) { return &f->s.nodes[slot].computed; }

static void props(fixture *f, uint32_t slot, const ui_style_decl *d, uint32_t n) {
    CHECK(ui_cmd_set_props(f->w, h(slot), d, n) == UI_OK);
}

static bool green(const float *c) { return c[1] == 1 && c[0] == 0 && c[2] == 0; }
static bool blue(const float *c) { return c[2] == 1 && c[0] == 0 && c[1] == 0; }

// 1 (root) > 2 > {3, 4 > 5}; 4 declares its own text color.
static void tree(fixture *f) {
    setup(f);
    ui_cmd_create_node(f->w, h(2), h(1));
    ui_cmd_create_node(f->w, h(3), h(2));
    ui_cmd_create_node(f->w, h(4), h(2));
    ui_cmd_create_node(f->w, h(5), h(4));
    props(f, 4, &(ui_style_decl){UI_STYLE_TEXT_COLOR, ui_rgba(0, 0, 1, 1)}, 1);
}

// Text values flow down to every node that doesn't declare its own; a change computes only
// the nodes whose inherited values change, and stops where a node declares its own.
static void test_inheritance(void) {
    fixture f;
    tree(&f);
    props(&f, 1, (ui_style_decl[]){{UI_STYLE_TEXT_COLOR, ui_rgba(0, 1, 0, 1)}, {UI_STYLE_FONT_SIZE, ui_px(20)},
                                   {UI_STYLE_FONT_FAMILY, ui_string("Noto Sans Hebrew")},
                                   {UI_STYLE_FONT_WEIGHT, ui_number(700)},
                                   {UI_STYLE_DIRECTION, ui_keyword(UI_DIRECTION_RTL)},
                                   {UI_STYLE_BACKGROUND_COLOR, ui_rgba(1, 0, 0, 1)}},
          6);
    flush(&f);
    CHECK(green(cv(&f, 2)->text.color) && green(cv(&f, 3)->text.color));
    CHECK(blue(cv(&f, 4)->text.color) && blue(cv(&f, 5)->text.color));
    CHECK(cv(&f, 5)->text.size == 20 && cv(&f, 5)->text.weight == 700);
    CHECK(cv(&f, 5)->text.direction == UI_DIRECTION_RTL);
    CHECK(cv(&f, 5)->text.family && !strcmp(cv(&f, 5)->text.family, "Noto Sans Hebrew"));
    CHECK(cv(&f, 3)->text.family == cv(&f, 1)->text.family); // one interned string
    CHECK(cv(&f, 2)->background[3] == 0); // backgrounds don't inherit
    uint64_t before = f.s.stats.styles_computed;
    props(&f, 1, &(ui_style_decl){UI_STYLE_TEXT_COLOR, ui_rgba(0, 0, 1, 1)}, 1);
    flush(&f);
    // 1, 2, 3 change; 4 is computed and found unchanged, so 5 isn't.
    CHECK_EQ_INT(f.s.stats.styles_computed - before, 4);
    CHECK(blue(cv(&f, 3)->text.color));
    // A paint change on the root computes the root alone.
    before = f.s.stats.styles_computed;
    props(&f, 1, &(ui_style_decl){UI_STYLE_BACKGROUND_COLOR, ui_rgba(0, 1, 0, 1)}, 1);
    flush(&f);
    CHECK_EQ_INT(f.s.stats.styles_computed - before, 1);
    // A moved node inherits from its new parent.
    props(&f, 2, &(ui_style_decl){UI_STYLE_FONT_SIZE, ui_px(9)}, 1);
    ui_cmd_create_node(f.w, h(6), f.surface);
    ui_cmd_move_node(f.w, h(3), h(6), (ui_handle){0});
    flush(&f);
    CHECK(cv(&f, 5)->text.size == 9 && cv(&f, 3)->text.size == 16); // 6 is at the surface: initial
    CHECK(cv(&f, 3)->text.direction == UI_DIRECTION_LTR);
    teardown(&f);
}

// inherit and initial on any property: a non-inherited property that takes inherit follows
// its parent; initial resets an inherited one for the node and its descendants.
static void test_inherit_initial(void) {
    fixture f;
    tree(&f);
    props(&f, 1, (ui_style_decl[]){{UI_STYLE_BACKGROUND_COLOR, ui_rgba(0, 1, 0, 1)},
                                   {UI_STYLE_TEXT_COLOR, ui_rgba(0, 1, 0, 1)},
                                   {UI_STYLE_PADDING_LEFT, ui_percent(10)}},
          3);
    ui_style_decl kid[] = {{UI_STYLE_BACKGROUND_COLOR, ui_inherit()}, {UI_STYLE_PADDING_LEFT, ui_inherit()},
                           {UI_STYLE_TEXT_COLOR, ui_initial()}};
    define(&f, 9, &(ui_style_block_desc){0, kid, 3}, 1);
    ui_cmd_set_styles(f.w, h(2), (uint32_t[]){9}, 1);
    flush(&f);
    CHECK(green(cv(&f, 2)->background));
    CHECK(cv(&f, 2)->layout.padding[3].unit == UI_LEN_PERCENT && cv(&f, 2)->layout.padding[3].value == 10);
    const float *t = cv(&f, 3)->text.color;
    CHECK(t[0] == 0 && t[1] == 0 && t[2] == 0 && t[3] == 1); // initial: opaque black, inherited by 3
    // The parent's background changes: the child that inherits it follows.
    props(&f, 1, &(ui_style_decl){UI_STYLE_BACKGROUND_COLOR, ui_rgba(0, 0, 1, 1)}, 1);
    flush(&f);
    CHECK(blue(cv(&f, 2)->background) && cv(&f, 3)->background[3] == 0);
    // Transitions inherit too.
    props(&f, 1, &(ui_style_decl){UI_STYLE_TRANSITION(UI_STYLE_OPACITY), ui_transition(50, 5)}, 1);
    props(&f, 2, &(ui_style_decl){UI_STYLE_TRANSITION(UI_STYLE_OPACITY), ui_inherit()}, 1);
    flush(&f);
    CHECK(cv(&f, 2)->transition[UI_ANIM_OPACITY][0] == 50);
    teardown(&f);
}

// set_text's values stand in for the inherited ones; the node's styles still override them.
static void test_text_inheritance(void) {
    fixture f;
    tree(&f);
    props(&f, 1, (ui_style_decl[]){{UI_STYLE_TEXT_COLOR, ui_rgba(0, 1, 0, 1)}, {UI_STYLE_LINE_HEIGHT, ui_number(2)}},
          2);
    static const char *const fam[] = {"Noto Sans"};
    ui_text_desc t = {.set = UI_TEXT_SET_ALL, .text = "Hi", .length = 2, .families = fam, .family_count = 1,
                      .style = {.family = 0, .weight = 300, .size = 11, .color = {0, 0, 1, 1}}};
    ui_cmd_set_text(f.w, h(3), &t);
    flush(&f);
    const ui_computed *c = cv(&f, 3);
    CHECK(blue(c->text.color) && c->text.size == 11 && c->text.weight == 300);
    CHECK(c->text.line_height == 0); // set_text's (0: from the fonts) replaces the inherited 2
    CHECK(c->text.family && !strcmp(c->text.family, "Noto Sans"));
    CHECK(blue(f.s.nodes[3].text_color));
    props(&f, 3, &(ui_style_decl){UI_STYLE_TEXT_COLOR, ui_inherit()}, 1); // the parent's again
    flush(&f);
    CHECK(green(cv(&f, 3)->text.color) && green(f.s.nodes[3].text_color));
    teardown(&f);
}

// Strings are interned and freed once nothing refers to them; tokens can hold them.
static void test_strings(void) {
    fixture f;
    tree(&f);
    uint32_t base = f.s.styles.string_live;
    ui_cmd_set_token(f.w, 3, ui_string("Serif Family"));
    props(&f, 1, &(ui_style_decl){UI_STYLE_FONT_FAMILY, ui_token_ref(3)}, 1);
    props(&f, 2, &(ui_style_decl){UI_STYLE_LANGUAGE, ui_string("sr-Cyrl")}, 1);
    flush(&f);
    CHECK(!strcmp(cv(&f, 5)->text.family, "Serif Family") && !strcmp(cv(&f, 5)->text.language, "sr-Cyrl"));
    CHECK_EQ_INT(f.s.styles.string_live, base + 2);
    ui_cmd_set_token(f.w, 3, ui_string("Sans Family"));
    props(&f, 2, &(ui_style_decl){UI_STYLE_LANGUAGE, ui_none()}, 1);
    flush(&f);
    CHECK(!strcmp(cv(&f, 5)->text.family, "Sans Family") && !cv(&f, 5)->text.language);
    CHECK_EQ_INT(f.s.styles.string_live, base + 1); // the old family and the language are gone
    // The writer refuses what the reader would drop.
    ui_cmd_writer *w = f.w;
    CHECK(ui_cmd_set_props(w, h(1), &(ui_style_decl){UI_STYLE_FONT_FAMILY, ui_string("")}, 1) ==
          UI_ERR_INVALID_ARGUMENT);
    CHECK(ui_cmd_set_props(w, h(1), &(ui_style_decl){UI_STYLE_FONT_FAMILY, {UI_VALUE_STRING, .string = {"a\0b", 3}}},
                           1) == UI_ERR_INVALID_ARGUMENT);
    CHECK(ui_cmd_set_props(w, h(1), &(ui_style_decl){UI_STYLE_FONT_FAMILY, {UI_VALUE_STRING, .string = {"\xff", 1}}},
                           1) == UI_ERR_INVALID_ARGUMENT);
    CHECK(ui_cmd_set_props(w, h(1), &(ui_style_decl){UI_STYLE_FONT_SIZE, ui_string("12")}, 1) ==
          UI_ERR_INVALID_ARGUMENT);
    CHECK(ui_cmd_set_props(w, h(1), &(ui_style_decl){UI_STYLE_LAYOUT, ui_inherit()}, 1) == UI_ERR_INVALID_ARGUMENT);
    CHECK(ui_cmd_set_token(w, 4, ui_inherit()) == UI_ERR_INVALID_ARGUMENT);
    teardown(&f);
}

// Logical sides map with the node's own direction; the later of a logical and a physical
// declaration wins, within a block and across the cascade; a direction inherited anew maps
// them again.
static void test_logical(void) {
    fixture f;
    setup(&f);
    ui_cmd_create_node(f.w, h(2), h(1));
    props(&f, 1, &(ui_style_decl){UI_STYLE_DIRECTION, ui_keyword(UI_DIRECTION_RTL)}, 1);
    ui_style_decl block[] = {{UI_STYLE_MARGIN_RIGHT, ui_px(40)}, {UI_STYLE_MARGIN_START, ui_px(10)},
                             {UI_STYLE_PADDING_START, ui_px(3)}, {UI_STYLE_PADDING_LEFT, ui_px(9)},
                             {UI_STYLE_INSET_END, ui_percent(5)}};
    define(&f, 4, &(ui_style_block_desc){0, block, 5}, 1);
    ui_cmd_set_styles(f.w, h(2), (uint32_t[]){4}, 1);
    flush(&f);
    const ui_layout_style *l = &cv(&f, 2)->layout;
    // rtl: start is right, end is left.
    CHECK(l->margin[1].value == 10 && l->margin[3].unit == UI_LEN_PX && l->margin[3].value == 0); // later start wins
    CHECK(l->padding[3].value == 9 && l->padding[1].value == 3); // later left wins over start (= right)
    CHECK(l->inset[3].unit == UI_LEN_PERCENT && l->inset[3].value == 5);
    // The parent turns ltr: the same cascade maps the other way.
    props(&f, 1, &(ui_style_decl){UI_STYLE_DIRECTION, ui_keyword(UI_DIRECTION_LTR)}, 1);
    flush(&f);
    CHECK(l->margin[3].value == 10 && l->margin[1].value == 40); // start = left; right stays 40
    CHECK(l->padding[3].value == 9 && l->padding[1].value == 0); // left 9 came after start (= left)
    CHECK(l->inset[1].unit == UI_LEN_PERCENT && l->inset[3].unit == UI_LEN_AUTO);
    // Across the cascade: an override's physical side beats the style's logical one.
    props(&f, 2, &(ui_style_decl){UI_STYLE_MARGIN_LEFT, ui_px(1)}, 1);
    flush(&f);
    CHECK(l->margin[3].value == 1);
    // An invalid token makes a logical declaration absent: an earlier one shows through.
    ui_style_decl more[] = {{UI_STYLE_MARGIN_END, ui_px(7)}, {UI_STYLE_MARGIN_END, ui_token_ref(99)}};
    props(&f, 2, &(ui_style_decl){UI_STYLE_MARGIN_LEFT, ui_unset()}, 1);
    define(&f, 5, &(ui_style_block_desc){0, more, 2}, 1);
    ui_cmd_set_styles(f.w, h(2), (uint32_t[]){4, 5}, 2);
    flush(&f);
    CHECK(l->margin[1].value == 7); // end = right in ltr
    teardown(&f);
}

int main(void) {
    RUN(test_logical);
    RUN(test_inheritance);
    RUN(test_inherit_initial);
    RUN(test_text_inheritance);
    RUN(test_strings);
    RUN(test_precedence);
    RUN(test_state_specificity);
    RUN(test_tokens);
    RUN(test_transitions);
    RUN(test_legacy_ops);
    RUN(test_paint_values);
    RUN(test_text_values);
    return test_failures != 0;
}
