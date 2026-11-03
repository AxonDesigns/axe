// Layout on the scene (docs/layout.md): the surface root, set_rect as an absolute frame,
// display: none, move_node, text leaves, rect springs, and incremental relayout (only what
// changed is computed again, the rest comes from the caches).
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "core/scene.h"
#include "test.h"
#include "ui/ui.h"

#define FONT(name) UI_TEST_FONTS "/" name

static ui_logger quiet = {UI_LOG_ERROR};
static ui_font_registry *fonts;
static ui_text_context text;

typedef struct fixture {
    ui_scene s;
    ui_cmd_writer *w;
    ui_handle surfaces[2];
    float width, height;
    uint64_t now;
} fixture;

static void setup(fixture *f, float width, float height) {
    memset(f, 0, sizeof *f);
    ui_scene_init(&f->s, &quiet);
    f->surfaces[0] = (ui_handle){(uint32_t)UI_HANDLE_SURFACE << UI_HANDLE_SLOT_BITS, 1};
    f->surfaces[1] = (ui_handle){(uint32_t)UI_HANDLE_SURFACE << UI_HANDLE_SLOT_BITS | 1, 1};
    ui_scene_add_root(&f->s, f->surfaces[0]);
    ui_scene_add_root(&f->s, f->surfaces[1]);
    ui_cmd_writer_create(&f->w);
    f->width = width;
    f->height = height;
}

static void teardown(fixture *f) {
    ui_cmd_writer_destroy(f->w);
    ui_scene_destroy(&f->s);
}

// Applies what was written, resolves, animates and lays out both surfaces.
static void frame(fixture *f) {
    uint8_t *bytes;
    size_t size;
    if (ui_cmd_writer_op_count(f->w)) {
        ui_cmd_writer_take(f->w, &bytes, &size);
        CHECK(ui_scene_apply(&f->s, bytes, size, f->now) == UI_OK);
        free(bytes);
    }
    ui_scene_resolve(&f->s, f->now);
    ui_scene_animate(&f->s, f->now);
    for (int i = 0; i < 2; i++) ui_scene_layout(&f->s, f->surfaces[i], f->width, f->height, 1, &text, f->now);
}

static ui_handle node(uint32_t slot) { return (ui_handle){slot, 1}; }

static const float *rect(fixture *f, uint32_t slot) { return f->s.nodes[slot].layout.rect; }

static bool rect_is(fixture *f, uint32_t slot, float x, float y, float w, float h) {
    const float *r = rect(f, slot);
    bool ok = fabsf(r[0] - x) < 1e-3f && fabsf(r[1] - y) < 1e-3f && fabsf(r[2] - w) < 1e-3f &&
              fabsf(r[3] - h) < 1e-3f;
    if (!ok)
        fprintf(stderr, "    node %u: %g %g %g %g, want %g %g %g %g\n", slot, (double)r[0], (double)r[1],
                (double)r[2], (double)r[3], (double)x, (double)y, (double)w, (double)h);
    return ok;
}

static void props(fixture *f, uint32_t slot, const ui_style_decl *d, uint32_t n) {
    CHECK(ui_cmd_set_props(f->w, node(slot), d, n) == UI_OK);
}

static void test_root(void) {
    fixture f;
    setup(&f, 400, 300);
    // The surface is a column: top-level nodes stack and stretch to its width.
    ui_cmd_create_node(f.w, node(1), f.surfaces[0]);
    props(&f, 1, &(ui_style_decl){UI_STYLE_HEIGHT, ui_px(50)}, 1);
    ui_cmd_create_node(f.w, node(2), f.surfaces[0]);
    props(&f, 2, &(ui_style_decl){UI_STYLE_FLEX_GROW, ui_number(1)}, 1);
    frame(&f);
    CHECK(rect_is(&f, 1, 0, 0, 400, 50));
    CHECK(rect_is(&f, 2, 0, 50, 400, 250));
    // A resize lays out again.
    f.width = 200;
    frame(&f);
    CHECK(rect_is(&f, 2, 0, 50, 200, 250));
    teardown(&f);
}

// set_rect is an absolute frame in the parent's padding box: with no padding or border it
// is exactly the old behavior.
static void test_set_rect(void) {
    fixture f;
    setup(&f, 400, 300);
    ui_cmd_create_node(f.w, node(1), f.surfaces[0]);
    ui_cmd_set_rect(f.w, node(1), 10, 20, 200, 100);
    ui_cmd_create_node(f.w, node(2), node(1));
    ui_cmd_set_rect(f.w, node(2), 5, 6, 30, 40);
    ui_cmd_create_node(f.w, node(3), f.surfaces[0]); // in flow: unaffected by the frames
    props(&f, 3, &(ui_style_decl){UI_STYLE_HEIGHT, ui_px(20)}, 1);
    frame(&f);
    CHECK(rect_is(&f, 1, 10, 20, 200, 100));
    CHECK(rect_is(&f, 2, 5, 6, 30, 40));
    CHECK(rect_is(&f, 3, 0, 0, 400, 20));
    CHECK(memcmp(f.s.nodes[2].rect, rect(&f, 2), sizeof f.s.nodes[2].rect) == 0); // drawn there
    // With a border on the parent, the frame is inside it (its padding box), as in CSS.
    props(&f, 1, &(ui_style_decl){UI_STYLE_BORDER_WIDTH, ui_px(4)}, 1);
    frame(&f);
    CHECK(rect_is(&f, 2, 9, 10, 30, 40));
    // Clearing the frame's overrides puts the node back in the flow.
    ui_style_decl clear[] = {{UI_STYLE_POSITION, ui_unset()}, {UI_STYLE_LEFT, ui_unset()},
                             {UI_STYLE_TOP, ui_unset()}, {UI_STYLE_WIDTH, ui_unset()}};
    props(&f, 2, clear, 4);
    frame(&f);
    CHECK(rect_is(&f, 2, 4, 4, 0, 40)); // row item: content width 0, its height override stays
    teardown(&f);
}

static void test_display_none(void) {
    fixture f;
    setup(&f, 300, 100);
    ui_cmd_create_node(f.w, node(1), f.surfaces[0]);
    props(&f, 1, (ui_style_decl[]){{UI_STYLE_FLEX_DIRECTION, ui_keyword(UI_FLEX_ROW)},
                                   {UI_STYLE_HEIGHT, ui_px(100)}}, 2);
    for (uint32_t i = 2; i <= 4; i++) {
        ui_cmd_create_node(f.w, node(i), node(1));
        props(&f, i, &(ui_style_decl){UI_STYLE_FLEX_GROW, ui_number(1)}, 1);
    }
    frame(&f);
    CHECK(rect_is(&f, 3, 100, 0, 100, 100));
    props(&f, 3, &(ui_style_decl){UI_STYLE_DISPLAY, ui_keyword(UI_DISPLAY_NONE)}, 1);
    frame(&f);
    CHECK(rect_is(&f, 2, 0, 0, 150, 100));
    CHECK(rect_is(&f, 3, 0, 0, 0, 0));
    CHECK(rect_is(&f, 4, 150, 0, 150, 100));
    // Hidden nodes aren't drawn, with their subtree.
    ui_cmd_set_color(f.w, node(3), 1, 0, 0, 1);
    frame(&f);
    ui_display_list dl = {.scale_120 = 120, .width = 300, .height = 100};
    ui_display_list_reset(&dl);
    CHECK(ui_scene_build(&f.s, f.surfaces[0], &dl, NULL) == UI_OK);
    CHECK_EQ_INT(dl.rect_count, 0);
    ui_display_list_free(&dl);
    // Shown again, it takes its share back. Regression: a hidden node is never computed, so
    // it stayed dirty, and showing it didn't reach its parent (propagation stops at the
    // first dirty node), whose cached layout then skipped it.
    props(&f, 3, &(ui_style_decl){UI_STYLE_DISPLAY, ui_unset()}, 1);
    frame(&f);
    CHECK(rect_is(&f, 3, 100, 0, 100, 100) && rect_is(&f, 4, 200, 0, 100, 100));
    teardown(&f);
}

static void test_move_node(void) {
    fixture f;
    setup(&f, 300, 100);
    ui_cmd_create_node(f.w, node(1), f.surfaces[0]);
    props(&f, 1, &(ui_style_decl){UI_STYLE_HEIGHT, ui_px(50)}, 1);
    for (uint32_t i = 2; i <= 4; i++) {
        ui_cmd_create_node(f.w, node(i), node(1));
        props(&f, i, &(ui_style_decl){UI_STYLE_WIDTH, ui_px(10.f * (float)i)}, 1);
    }
    frame(&f);
    CHECK(rect_is(&f, 2, 0, 0, 20, 50) && rect_is(&f, 3, 20, 0, 30, 50) && rect_is(&f, 4, 50, 0, 40, 50));
    // Reorder: 4 before 2.
    CHECK(ui_cmd_move_node(f.w, node(4), node(1), node(2)) == UI_OK);
    frame(&f);
    CHECK(rect_is(&f, 4, 0, 0, 40, 50) && rect_is(&f, 2, 40, 0, 20, 50) && rect_is(&f, 3, 60, 0, 30, 50));
    // Append: 2 to the end.
    ui_cmd_move_node(f.w, node(2), node(1), (ui_handle){0});
    frame(&f);
    CHECK(rect_is(&f, 3, 40, 0, 30, 50) && rect_is(&f, 2, 70, 0, 20, 50));
    // To another surface: a top-level node there now.
    ui_cmd_move_node(f.w, node(3), f.surfaces[1], (ui_handle){0});
    frame(&f);
    CHECK(f.s.nodes[3].root == 1 && rect_is(&f, 3, 0, 0, 30, 0)); // its own width, no content
    CHECK(rect_is(&f, 2, 40, 0, 20, 50));
    CHECK_EQ_INT(f.s.stats.dropped, 0);
    // Refused: under itself or a descendant, before a node with another parent, stale.
    ui_cmd_create_node(f.w, node(5), node(4));
    ui_cmd_move_node(f.w, node(1), node(5), (ui_handle){0});
    ui_cmd_move_node(f.w, node(1), node(1), (ui_handle){0});
    ui_cmd_move_node(f.w, node(2), node(1), node(5));
    ui_cmd_move_node(f.w, (ui_handle){9, 1}, node(1), (ui_handle){0});
    frame(&f);
    CHECK_EQ_INT(f.s.stats.dropped, 4);
    CHECK(f.s.nodes[1].parent == UI_NO_SLOT && f.s.nodes[2].parent == 1);
    teardown(&f);
}

static void set_text(fixture *f, uint32_t slot, const char *s) {
    static const char *const fam[] = {"Noto Sans"};
    ui_text_desc t = {.set = UI_TEXT_SET_ALL, .text = s, .length = (uint32_t)strlen(s),
                      .style = {.family = 0, .weight = 400, .size = 16, .color = {0, 0, 0, 1}},
                      .families = fam, .family_count = 1, .line_height = 1.25f};
    CHECK(ui_cmd_set_text(f->w, node(slot), &t) == UI_OK);
}

// A text leaf sizes itself from its text; its text fills its content box.
static void test_text(void) {
    fixture f;
    setup(&f, 300, 200);
    ui_cmd_create_node(f.w, node(1), f.surfaces[0]);
    props(&f, 1, &(ui_style_decl){UI_STYLE_ALIGN_ITEMS, ui_keyword(UI_FLEX_START)}, 1);
    ui_cmd_create_node(f.w, node(2), node(1));
    props(&f, 2, &(ui_style_decl){UI_STYLE_PADDING_LEFT, ui_px(10)}, 1);
    set_text(&f, 2, "Short");
    frame(&f);
    float short_w = rect(&f, 2)[2];
    CHECK(short_w > 10 && rect(&f, 2)[3] == 20);
    const float *t = f.s.nodes[2].layout.text;
    CHECK(t[0] == 10 && t[2] == short_w - 10);
    // Longer text: wider; text longer than the room wraps (two lines, 40 px).
    set_text(&f, 2, "A much longer line of text");
    frame(&f);
    CHECK(rect(&f, 2)[2] > short_w);
    set_text(&f, 2, "A much longer line of text that cannot fit in three hundred pixels of width");
    frame(&f);
    CHECK(rect(&f, 2)[2] == 300 && rect(&f, 2)[3] == 40);
    teardown(&f);
}

// start_spring on the rect moves set_rect's frame; layout follows every frame.
static void test_rect_spring(void) {
    fixture f;
    setup(&f, 400, 300);
    ui_cmd_create_node(f.w, node(1), f.surfaces[0]);
    ui_cmd_set_rect(f.w, node(1), 0, 0, 100, 50);
    ui_cmd_create_node(f.w, node(2), node(1));
    props(&f, 2, &(ui_style_decl){UI_STYLE_FLEX_GROW, ui_number(1)}, 1);
    frame(&f);
    CHECK(rect_is(&f, 2, 0, 0, 100, 50));
    float target[4] = {0, 0, 300, 50};
    ui_cmd_start_spring(f.w, node(1), UI_PROP_RECT, target, 100, 20);
    frame(&f);
    f.now += 100000000;
    frame(&f);
    float mid = rect(&f, 1)[2];
    CHECK(mid > 100 && mid < 300);
    CHECK(rect(&f, 2)[2] == mid); // the child grows with it
    f.now += 5000000000ull;
    frame(&f);
    CHECK(rect_is(&f, 1, 0, 0, 300, 50) && rect_is(&f, 2, 0, 0, 300, 50));
    teardown(&f);
}

// A tree of rows of boxes: a change computes its path again; untouched rows come from the
// caches; nothing changed computes nothing.
static void test_incremental(void) {
    fixture f;
    setup(&f, 400, 600);
    uint32_t slot = 1;
    ui_cmd_create_node(f.w, node(slot), f.surfaces[0]);
    uint32_t top = slot++;
    for (int r = 0; r < 20; r++) {
        uint32_t row = slot++;
        ui_cmd_create_node(f.w, node(row), node(top));
        props(&f, row, &(ui_style_decl){UI_STYLE_HEIGHT, ui_px(20)}, 1);
        for (int k = 0; k < 10; k++) {
            uint32_t leaf = slot++;
            ui_cmd_create_node(f.w, node(leaf), node(row));
            props(&f, leaf, &(ui_style_decl){UI_STYLE_FLEX_GROW, ui_number(1)}, 1);
        }
    }
    props(&f, top, &(ui_style_decl){UI_STYLE_FLEX_DIRECTION, ui_keyword(UI_FLEX_COLUMN)}, 1);
    frame(&f);
    uint64_t full = f.s.layout_stats.computed;
    CHECK(full >= 221);
    CHECK(rect_is(&f, 3, 0, 0, 40, 20));
    // Nothing changed: no layout at all.
    frame(&f);
    CHECK_EQ_INT(f.s.layout_stats.computed, full);
    // One leaf changes: its row and the ancestors run again; the other rows' are cached.
    uint64_t hits = f.s.layout_stats.cache_hits;
    props(&f, 3, &(ui_style_decl){UI_STYLE_FLEX_GROW, ui_number(3)}, 1);
    frame(&f);
    // The changed row lays its 10 leaves out again (their sizes changed), a few measures
    // each, then its ancestors; the other 19 rows come from their caches (about 840
    // computations lay out the whole tree, 26 this change).
    uint64_t again = f.s.layout_stats.computed - full;
    CHECK(again > 0 && again < 50);
    CHECK(f.s.layout_stats.cache_hits > hits);
    CHECK(rect_is(&f, 3, 0, 0, 400.f * 3 / 12, 20));
    // A paint change doesn't lay out.
    uint64_t before = f.s.layout_stats.computed;
    ui_cmd_set_color(f.w, node(5), 1, 0, 0, 1);
    frame(&f);
    CHECK_EQ_INT(f.s.layout_stats.computed, before);
    teardown(&f);
}

// Layout values through styles: tokens, state blocks and redefinitions lay out again.
static void test_styles(void) {
    fixture f;
    setup(&f, 300, 200);
    ui_cmd_set_token(f.w, 1, ui_px(100));
    ui_style_decl card[] = {{UI_STYLE_WIDTH, ui_token_ref(1)}, {UI_STYLE_HEIGHT, ui_px(40)}};
    ui_style_decl hover[] = {{UI_STYLE_PADDING_LEFT, ui_px(10)}, {UI_STYLE_PADDING_TOP, ui_percent(10)}};
    ui_style_block_desc blocks[] = {{0, card, 2}, {UI_STATE_HOVER, hover, 2}};
    CHECK(ui_cmd_define_style(f.w, 7, blocks, 2) == UI_OK);
    ui_cmd_create_node(f.w, node(1), f.surfaces[0]);
    props(&f, 1, &(ui_style_decl){UI_STYLE_ALIGN_ITEMS, ui_keyword(UI_FLEX_START)}, 1);
    ui_cmd_create_node(f.w, node(2), node(1));
    ui_cmd_set_styles(f.w, node(2), (uint32_t[]){7}, 1);
    ui_cmd_create_node(f.w, node(3), node(2));
    props(&f, 3, (ui_style_decl[]){{UI_STYLE_WIDTH, ui_px(5)}, {UI_STYLE_HEIGHT, ui_px(5)}}, 2);
    frame(&f);
    CHECK(rect_is(&f, 2, 0, 0, 100, 40) && rect_is(&f, 3, 0, 0, 5, 5));
    ui_cmd_set_token(f.w, 1, ui_px(150)); // a token
    frame(&f);
    CHECK(rect_is(&f, 2, 0, 0, 150, 40));
    ui_cmd_set_state(f.w, node(2), UI_STATE_HOVER, UI_STATE_HOVER); // a state block
    frame(&f);
    CHECK(rect_is(&f, 3, 10, 30, 5, 5)); // 10% of the containing block's width, 300
    ui_style_decl column[] = {{UI_STYLE_WIDTH, ui_px(60)}, {UI_STYLE_FLEX_DIRECTION, ui_keyword(UI_FLEX_COLUMN)},
                              {UI_STYLE_ALIGN_ITEMS, ui_keyword(UI_FLEX_CENTER)}};
    CHECK(ui_cmd_define_style(f.w, 7, &(ui_style_block_desc){0, column, 3}, 1) == UI_OK); // redefined
    frame(&f);
    CHECK(rect_is(&f, 2, 0, 0, 60, 5) && rect_is(&f, 3, 27.5f, 0, 5, 5));
    // Layout properties don't transition: geometry animates through layout animations.
    CHECK(ui_cmd_set_props(f.w, node(2), &(ui_style_decl){UI_STYLE_TRANSITION(UI_STYLE_WIDTH), ui_transition(100, 10)}, 1) ==
          UI_ERR_INVALID_ARGUMENT);
    CHECK_EQ_INT(f.s.stats.dropped, 0);
    teardown(&f);
}

// Text that leaves its font to the node follows the node's styles and its parent's: the
// family, size and direction come from them, and a change of any reshapes it. Spans can
// leave single fields to the node too.
static void test_styled_text(void) {
    fixture f;
    setup(&f, 400, 200);
    ui_cmd_create_node(f.w, node(1), f.surfaces[0]);
    props(&f, 1, (ui_style_decl[]){{UI_STYLE_ALIGN_ITEMS, ui_keyword(UI_FLEX_START)},
                                   {UI_STYLE_FONT_FAMILY, ui_string("Noto Sans")},
                                   {UI_STYLE_FONT_SIZE, ui_px(16)},
                                   {UI_STYLE_LINE_HEIGHT, ui_number(1.25f)}},
          4);
    ui_cmd_create_node(f.w, node(2), node(1));
    ui_text_span_desc span = {0, 1, {.weight = 700, .size = 40}, UI_TEXT_SET_WEIGHT}; // size from the node
    ui_text_desc t = {.text = "Styled", .length = 6, .spans = &span, .span_count = 1}; // nothing set
    CHECK(ui_cmd_set_text(f.w, node(2), &t) == UI_OK);
    frame(&f);
    const ui_text_source *src = f.s.nodes[2].text->source;
    CHECK(src->base.size == 16 && src->spans[0].style.size == 16 && src->spans[0].style.weight == 700);
    CHECK(src->dir == UI_BIDI_LTR);
    float w16 = rect(&f, 2)[2];
    CHECK(rect(&f, 2)[3] == 20); // 1.25 x 16
    // The parent's size doubles: the text is shaped again and twice as big.
    props(&f, 1, (ui_style_decl[]){{UI_STYLE_FONT_SIZE, ui_px(32)}, {UI_STYLE_DIRECTION, ui_keyword(UI_DIRECTION_RTL)}},
          2);
    frame(&f);
    src = f.s.nodes[2].text->source;
    CHECK(src->base.size == 32 && src->spans[0].style.size == 32 && src->dir == UI_BIDI_RTL);
    CHECK(rect(&f, 2)[3] == 40);
    CHECK_NEAR(rect(&f, 2)[2], 2 * w16, 1);
    // A style on the node beats both, and set_text's own value loses to it too.
    t.set = UI_TEXT_SET_SIZE;
    t.style.size = 10;
    CHECK(ui_cmd_set_text(f.w, node(2), &t) == UI_OK);
    frame(&f);
    CHECK(f.s.nodes[2].text->source->base.size == 10 && f.s.nodes[2].text->source->spans[0].style.size == 10);
    props(&f, 2, &(ui_style_decl){UI_STYLE_FONT_SIZE, ui_px(12)}, 1);
    frame(&f);
    CHECK(f.s.nodes[2].text->source->base.size == 12);
    // A paint change on the parent doesn't touch the text.
    const ui_text_source *before = f.s.nodes[2].text->source;
    props(&f, 1, &(ui_style_decl){UI_STYLE_BACKGROUND_COLOR, ui_rgba(1, 0, 0, 1)}, 1);
    frame(&f);
    CHECK(f.s.nodes[2].text->source == before);
    teardown(&f);
}

// A right-to-left surface: its top-level nodes inherit rtl, so the surface column puts
// them at its cross-start (the right), and text inside follows the node's direction unless
// set_text sets one.
static void test_rtl(void) {
    fixture f;
    setup(&f, 400, 200);
    ui_scene_set_root_direction(&f.s, f.surfaces[0], UI_DIRECTION_RTL);
    ui_cmd_create_node(f.w, node(1), f.surfaces[0]);
    props(&f, 1, (ui_style_decl[]){{UI_STYLE_WIDTH, ui_px(100)}, {UI_STYLE_HEIGHT, ui_px(50)},
                                   {UI_STYLE_ALIGN_ITEMS, ui_keyword(UI_FLEX_START)}},
          3);
    ui_cmd_create_node(f.w, node(2), node(1));
    props(&f, 2, (ui_style_decl[]){{UI_STYLE_WIDTH, ui_px(30)}, {UI_STYLE_MARGIN_START, ui_px(5)}}, 2);
    ui_cmd_create_node(f.w, node(3), node(1));
    ui_text_desc t = {.text = "abc", .length = 3, .set = UI_TEXT_SET_ALL & ~UI_TEXT_SET_DIRECTION,
                      .style = {.family = UI_TEXT_FAMILY_DEFAULT, .weight = 400, .size = 12, .color = {0, 0, 0, 1}}};
    CHECK(ui_cmd_set_text(f.w, node(3), &t) == UI_OK);
    frame(&f);
    CHECK(rect_is(&f, 1, 300, 0, 100, 50));
    CHECK(rect(&f, 2)[0] == 100 - 5 - 30); // the first item at the right, after its start margin
    CHECK(f.s.nodes[3].text->source->dir == UI_BIDI_RTL);
    // The surface turns ltr: everything lays out again, mirrored back.
    ui_scene_set_root_direction(&f.s, f.surfaces[0], UI_DIRECTION_LTR);
    frame(&f);
    CHECK(rect_is(&f, 1, 0, 0, 100, 50) && rect(&f, 2)[0] == 5);
    CHECK(f.s.nodes[3].text->source->dir == UI_BIDI_LTR);
    // A set_text that sets its direction keeps it.
    t.set = UI_TEXT_SET_ALL;
    t.direction = UI_TEXT_DIR_AUTO;
    CHECK(ui_cmd_set_text(f.w, node(3), &t) == UI_OK);
    frame(&f);
    CHECK(f.s.nodes[3].text->source->dir == UI_BIDI_AUTO);
    teardown(&f);
}

// Borders per side lay out per side, snapped to whole device pixels as browsers do; logical
// sides follow the direction; a scale change lays them out again.
static void test_borders(void) {
    fixture f;
    setup(&f, 300, 200);
    ui_cmd_create_node(f.w, node(1), f.surfaces[0]);
    props(&f, 1, (ui_style_decl[]){{UI_STYLE_ALIGN_ITEMS, ui_keyword(UI_FLEX_START)},
                                   {UI_STYLE_BORDER_LEFT_WIDTH, ui_px(10)}, {UI_STYLE_BORDER_TOP_WIDTH, ui_px(3)},
                                   {UI_STYLE_BORDER_END_WIDTH, ui_px(1)}, {UI_STYLE_HEIGHT, ui_px(100)}},
          5);
    ui_cmd_create_node(f.w, node(2), node(1));
    props(&f, 2, (ui_style_decl[]){{UI_STYLE_WIDTH, ui_px(20)}, {UI_STYLE_HEIGHT, ui_px(20)}}, 2);
    frame(&f);
    CHECK(rect_is(&f, 1, 0, 0, 300, 100) && rect_is(&f, 2, 10, 3, 20, 20)); // left 10, end (right) 1
    CHECK(f.s.nodes[1].computed.border_width[1] == 1);
    // Right to left: the end side is the left one, and the later declaration wins it (1 px);
    // the right side has none. The first item sits at the right edge.
    props(&f, 1, &(ui_style_decl){UI_STYLE_DIRECTION, ui_keyword(UI_DIRECTION_RTL)}, 1);
    frame(&f);
    CHECK(f.s.nodes[1].computed.border_width[3] == 1 && f.s.nodes[1].computed.border_width[1] == 0);
    CHECK(rect_is(&f, 2, 280, 3, 20, 20));
    // At 1.25x a 3 px border is 3.75 device px, snapped to 3: 2.4 logical px.
    for (int i = 0; i < 2; i++) ui_scene_layout(&f.s, f.surfaces[i], f.width, f.height, 1.25f, &text, f.now);
    CHECK(rect_is(&f, 2, 280, 2.4f, 20, 20));
    CHECK_NEAR(ui_layout_snap_border(0.2f, 1.25f), 0.8, 1e-6); // nonzero: at least a device pixel
    CHECK(ui_layout_snap_border(0, 2) == 0);
    teardown(&f);
}

// A node with text and in-flow children is a mistake: counted once (and warned about in
// debug builds), however many frames lay it out. Absolute children are fine.
static void test_text_children(void) {
    fixture f;
    setup(&f, 300, 200);
    ui_cmd_create_node(f.w, node(1), f.surfaces[0]);
    set_text(&f, 1, "Label");
    ui_cmd_create_node(f.w, node(2), node(1));
    props(&f, 2, (ui_style_decl[]){{UI_STYLE_POSITION, ui_keyword(UI_POSITION_ABSOLUTE)},
                                   {UI_STYLE_WIDTH, ui_px(8)}, {UI_STYLE_HEIGHT, ui_px(8)}}, 3);
    frame(&f);
    CHECK_EQ_INT(f.s.stats.text_nodes_with_children, 0); // an overlay
    ui_cmd_create_node(f.w, node(3), node(1));
    props(&f, 3, &(ui_style_decl){UI_STYLE_WIDTH, ui_px(20)}, 1);
    frame(&f);
    CHECK_EQ_INT(f.s.stats.text_nodes_with_children, 1);
    set_text(&f, 1, "Label changed"); // laid out again: still counted once
    frame(&f);
    CHECK_EQ_INT(f.s.stats.text_nodes_with_children, 1);
    ui_cmd_create_node(f.w, node(4), f.surfaces[0]);
    set_text(&f, 4, "Another");
    ui_cmd_create_node(f.w, node(5), node(4));
    frame(&f);
    CHECK_EQ_INT(f.s.stats.text_nodes_with_children, 2);
    teardown(&f);
}

int main(void) {
    ui_font_registry_create(&(ui_font_registry_desc){.log = &quiet}, &fonts);
    ui_font_registry_add_file(fonts, FONT("NotoSans-Regular-Static.ttf"), 0, NULL);
    ui_text_context_init(&text, fonts);
    RUN(test_root);
    RUN(test_set_rect);
    RUN(test_display_none);
    RUN(test_move_node);
    RUN(test_text);
    RUN(test_rect_spring);
    RUN(test_incremental);
    RUN(test_styles);
    RUN(test_styled_text);
    RUN(test_rtl);
    RUN(test_borders);
    RUN(test_text_children);
    ui_text_context_free(&text);
    ui_font_registry_destroy(fonts);
    return test_failures != 0;
}
