// Layout animations: a node with a layout transition springs from where it is drawn (its
// presentation rect) to each new layout rect, keeping its velocity when the target moves
// again. Children follow their parent's presentation; move_node keeps a node where it is
// drawn; new and re-shown nodes appear in place. The scene-level tests drive the clock by
// hand; the app-level ones check both schedulers draw the same frames mid-animation, and
// golden images show content clipped or reflowed in a box that is still shrinking.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "core/scene.h"
#include "golden.h"

static ui_logger quiet = {UI_LOG_ERROR};

#define K 100.f // stiffness and damping of the layout transition: critically damped
#define D 20.f
#define MS 1000000ull

typedef struct fixture {
    ui_scene s;
    ui_cmd_writer *w;
    ui_handle surface;
    uint64_t now;
} fixture;

static ui_handle node(uint32_t slot) { return (ui_handle){slot, 1}; }

static void props(fixture *f, uint32_t slot, const ui_style_decl *d, uint32_t n) {
    CHECK(ui_cmd_set_props(f->w, node(slot), d, n) == UI_OK);
}

static void size(fixture *f, uint32_t slot, float w, float h) {
    props(f, slot, (ui_style_decl[]){{UI_STYLE_WIDTH, ui_px(w)}, {UI_STYLE_HEIGHT, ui_px(h)}}, 2);
}

static void animated(fixture *f, uint32_t slot) {
    props(f, slot, &(ui_style_decl){UI_STYLE_TRANSITION(UI_STYLE_LAYOUT), ui_transition(K, D)}, 1);
}

// Applies what was written, then resolves, animates and lays out at the current time, as
// a core step and its commit do.
static void frame(fixture *f) {
    uint8_t *bytes;
    size_t sz;
    if (ui_cmd_writer_op_count(f->w)) {
        ui_cmd_writer_take(f->w, &bytes, &sz);
        CHECK(ui_scene_apply(&f->s, bytes, sz, f->now) == UI_OK);
        free(bytes);
    }
    ui_scene_resolve(&f->s, f->now);
    CHECK(ui_scene_check_resolved(&f->s)); // as resolving every node from scratch
    ui_scene_animate(&f->s, f->now);
    ui_scene_layout(&f->s, f->surface, 400, 300, 1, NULL, f->now);
}

// A row (1) of two boxes: A (2) and B (3), 50 x 20 each; B has a layout transition.
static void setup(fixture *f) {
    memset(f, 0, sizeof *f);
    ui_scene_init(&f->s, &quiet);
    f->surface = (ui_handle){(uint32_t)UI_HANDLE_SURFACE << UI_HANDLE_SLOT_BITS, 1};
    ui_scene_add_root(&f->s, f->surface);
    ui_cmd_writer_create(&f->w);
    ui_cmd_create_node(f->w, node(1), f->surface);
    props(f, 1, &(ui_style_decl){UI_STYLE_ALIGN_ITEMS, ui_keyword(UI_FLEX_START)}, 1);
    for (uint32_t slot = 2; slot <= 3; slot++) {
        ui_cmd_create_node(f->w, node(slot), node(1));
        size(f, slot, 50, 20);
    }
    animated(f, 3);
    frame(f);
}

static void teardown(fixture *f) {
    ui_cmd_writer_destroy(f->w);
    ui_scene_destroy(&f->s);
}

static const float *shown(fixture *f, uint32_t slot) { return f->s.nodes[slot].rect; }
static const float *laid(fixture *f, uint32_t slot) { return f->s.nodes[slot].layout.rect; }

static ui_spring *spring(fixture *f, uint32_t slot) {
    ui_node_anim *a = f->s.nodes[slot].anim;
    return a ? &a->springs[UI_SPRING_LAYOUT] : NULL;
}

static bool moving(fixture *f, uint32_t slot) { return spring(f, slot) && spring(f, slot)->active; }

static bool same_rect(const float *a, const float *b) { return !memcmp(a, b, 4 * sizeof *a); }

// The node's presentation origin on the surface.
static void on_surface(fixture *f, uint32_t slot, float out[2]) {
    out[0] = out[1] = 0;
    for (; slot != UI_NO_SLOT; slot = f->s.nodes[slot].parent) {
        out[0] += f->s.nodes[slot].rect[0];
        out[1] += f->s.nodes[slot].rect[1];
    }
}

static void test_moves(void) {
    fixture f;
    setup(&f);
    // A new node appears at its layout rect.
    CHECK(same_rect(shown(&f, 3), (float[]){50, 0, 50, 20}) && !moving(&f, 3));
    size(&f, 2, 150, 20); // B's layout moves to x = 150
    frame(&f);
    CHECK(same_rect(shown(&f, 2), laid(&f, 2))); // A has no transition: it jumps
    CHECK(laid(&f, 3)[0] == 150 && shown(&f, 3)[0] == 50 && moving(&f, 3));
    CHECK(f.s.roots[0].animating); // frames keep coming
    f.now += 100 * MS;
    frame(&f);
    double x, v;
    ui_spring_solve(K, D, 50 - 150, 0, 0.1, &x, &v);
    CHECK_NEAR(shown(&f, 3)[0], 150 + x, 1e-3);
    CHECK(shown(&f, 3)[2] == 50); // the size didn't change
    f.now += 3000 * MS;
    frame(&f);
    CHECK(same_rect(shown(&f, 3), (float[]){150, 0, 50, 20}) && !moving(&f, 3));
    CHECK(!f.s.roots[0].animating);
    teardown(&f);
}

// A new target mid-flight keeps the position and velocity: the motion continues smoothly
// from where it was, by the closed form.
static void test_retarget(void) {
    fixture f;
    setup(&f);
    size(&f, 2, 150, 20);
    frame(&f);
    f.now += 50 * MS;
    frame(&f);
    double x1 = spring(&f, 3)->position[0], v1 = spring(&f, 3)->velocity[0];
    CHECK(x1 > 50 && x1 < 150 && v1 > 0);
    size(&f, 2, 0, 20); // back to x = 0, at the same instant
    frame(&f);
    CHECK(spring(&f, 3)->position[0] == x1 && spring(&f, 3)->velocity[0] == v1);
    CHECK(spring(&f, 3)->target[0] == 0);
    f.now += 16 * MS;
    frame(&f);
    double x, v;
    ui_spring_solve(K, D, x1, v1, 0.016, &x, &v);
    CHECK_NEAR(shown(&f, 3)[0], x, 1e-3);
    CHECK(shown(&f, 3)[0] > x1); // still carried right by its velocity
    teardown(&f);
}

// Children are placed in their parent's presentation rect: they move with it, at their own
// layout rects, and a parent's size animates while its children keep their final layout.
static void test_children_follow(void) {
    fixture f;
    setup(&f);
    props(&f, 3, &(ui_style_decl){UI_STYLE_PADDING_LEFT, ui_px(5)}, 1);
    ui_cmd_create_node(f.w, node(4), node(3));
    size(&f, 4, 10, 10);
    ui_cmd_set_color(f.w, node(3), 1, 0, 0, 1);
    ui_cmd_set_color(f.w, node(4), 0, 0, 1, 1);
    frame(&f);
    size(&f, 2, 150, 20); // B moves
    size(&f, 3, 90, 20);  // and grows
    frame(&f);
    ui_display_list dl = {.scale_120 = 150, .width = 500, .height = 375};
    bool fractional = false;
    for (int i = 0; i < 20; i++) {
        f.now += 16 * MS;
        frame(&f);
        CHECK(same_rect(shown(&f, 4), (float[]){5, 0, 10, 10})); // C keeps its layout
        CHECK(shown(&f, 3)[2] > 50 && shown(&f, 3)[2] < 90);
        ui_display_list_reset(&dl);
        CHECK(ui_scene_build(&f.s, f.surface, &dl, NULL) == UI_OK && dl.rect_count == 2);
        // While B moves, neither snaps: C stays exactly 5 px (6.25 physical) inside B.
        CHECK_NEAR(dl.rects[1].x - dl.rects[0].x, 5 * 1.25, 1e-3);
        fractional |= dl.rects[0].x != floorf(dl.rects[0].x);
    }
    CHECK(fractional);
    f.now += 3000 * MS;
    frame(&f);
    ui_display_list_reset(&dl);
    CHECK(ui_scene_build(&f.s, f.surface, &dl, NULL) == UI_OK);
    CHECK_NEAR(dl.rects[0].x, 188, 0); // at rest: 150 * 1.25 = 187.5, snapped
    CHECK_NEAR(dl.rects[0].width, 113, 0); // 90 * 1.25 = 112.5
    ui_display_list_free(&dl);
    teardown(&f);
}

// move_node keeps the node where it is drawn, then animates it into its new place; moved
// again mid-flight, it continues from where it is with its velocity. Without a transition
// it jumps.
static void test_reparent(void) {
    fixture f;
    setup(&f); // P1 = 2 at x 0, P2 = 3 at x 50 (the transition on 3 is irrelevant here)
    size(&f, 2, 100, 50);
    size(&f, 3, 100, 50);
    ui_cmd_create_node(f.w, node(4), node(2));
    size(&f, 4, 20, 20);
    props(&f, 4, &(ui_style_decl){UI_STYLE_MARGIN_LEFT, ui_px(30)}, 1);
    animated(&f, 4);
    f.now += 3000 * MS; // let P2 settle at x = 100
    frame(&f);
    f.now += 3000 * MS;
    frame(&f);
    CHECK(shown(&f, 3)[0] == 100 && !moving(&f, 3));
    float before[2], after[2];
    on_surface(&f, 4, before);
    CHECK(before[0] == 30);
    ui_cmd_move_node(f.w, node(4), node(3), (ui_handle){0});
    frame(&f);
    on_surface(&f, 4, after);
    CHECK(after[0] == before[0] && after[1] == before[1]); // drawn where it was
    CHECK(shown(&f, 4)[0] == -70 && laid(&f, 4)[0] == 30 && moving(&f, 4));
    f.now += 60 * MS;
    frame(&f);
    on_surface(&f, 4, before);
    double v = spring(&f, 4)->velocity[0];
    CHECK(before[0] > 30 && before[0] < 130 && v > 0);
    ui_cmd_move_node(f.w, node(4), node(2), (ui_handle){0}); // back, mid-flight
    frame(&f);
    on_surface(&f, 4, after);
    CHECK_NEAR(after[0], before[0], 1e-4);
    CHECK(spring(&f, 4)->velocity[0] == v);
    f.now += 3000 * MS;
    frame(&f);
    on_surface(&f, 4, after);
    CHECK(after[0] == 30 && !moving(&f, 4));
    // Without a transition: straight to the new place.
    props(&f, 4, &(ui_style_decl){UI_STYLE_TRANSITION(UI_STYLE_LAYOUT), ui_none()}, 1);
    ui_cmd_move_node(f.w, node(4), node(3), (ui_handle){0});
    frame(&f);
    on_surface(&f, 4, after);
    CHECK(after[0] == 130 && !moving(&f, 4));
    teardown(&f);
}

// Removing the transition mid-flight ends the animation at the layout rect.
static void test_transition_removed(void) {
    fixture f;
    setup(&f);
    size(&f, 2, 150, 20);
    frame(&f);
    f.now += 50 * MS;
    frame(&f);
    CHECK(moving(&f, 3));
    props(&f, 3, &(ui_style_decl){UI_STYLE_TRANSITION(UI_STYLE_LAYOUT), ui_unset()}, 1);
    frame(&f);
    CHECK(!moving(&f, 3) && same_rect(shown(&f, 3), laid(&f, 3)) && laid(&f, 3)[0] == 150);
    teardown(&f);
}

// A node shown again appears at its layout rect: it doesn't animate from where it was hidden.
static void test_shown_again(void) {
    fixture f;
    setup(&f);
    props(&f, 3, &(ui_style_decl){UI_STYLE_DISPLAY, ui_keyword(UI_DISPLAY_NONE)}, 1);
    frame(&f);
    size(&f, 2, 120, 20);
    frame(&f);
    props(&f, 3, &(ui_style_decl){UI_STYLE_DISPLAY, ui_unset()}, 1);
    frame(&f);
    CHECK(same_rect(shown(&f, 3), (float[]){120, 0, 50, 20}) && !moving(&f, 3));
    teardown(&f);
}

// The layout rect is transitioned, never declared.
static void test_wire(void) {
    ui_cmd_writer *w;
    ui_cmd_writer_create(&w);
    ui_handle n = {1, 1};
    CHECK(ui_cmd_set_props(w, n, &(ui_style_decl){UI_STYLE_TRANSITION(UI_STYLE_LAYOUT), ui_transition(K, D)},
                           1) == UI_OK);
    CHECK(ui_cmd_set_props(w, n, &(ui_style_decl){UI_STYLE_LAYOUT, ui_px(1)}, 1) == UI_ERR_INVALID_ARGUMENT);
    ui_style_decl decl = {UI_STYLE_TRANSITION(UI_STYLE_LAYOUT), ui_transition(K, D)};
    CHECK(ui_cmd_define_style(w, 1, &(ui_style_block_desc){0, &decl, 1}, 1) == UI_OK);
    ui_cmd_writer_destroy(w);
}

// While a box's drawn size differs from its layout size, its children (and text) are
// clipped to its padding box as drawn; at rest, nothing is.
static void test_content_clip(void) {
    fixture f;
    setup(&f); // B (3) animates; give it a border and a child wider than it ends up
    props(&f, 3, &(ui_style_decl){UI_STYLE_BORDER_WIDTH, ui_px(2)}, 1);
    ui_cmd_create_node(f.w, node(4), node(3));
    props(&f, 4, (ui_style_decl[]){{UI_STYLE_WIDTH, ui_px(40)}, {UI_STYLE_HEIGHT, ui_px(10)},
                                   {UI_STYLE_FLEX_SHRINK, ui_number(0)}}, 3);
    ui_cmd_set_color(f.w, node(4), 1, 0, 0, 1);
    frame(&f);
    size(&f, 3, 20, 20); // shrinks
    frame(&f);
    f.now += 120 * MS;
    frame(&f);
    float w = shown(&f, 3)[2];
    CHECK(moving(&f, 3) && w > 20 && w < 40); // the child (40 wide) now overflows the padding box
    ui_display_list dl = {.scale_120 = 120, .width = 400, .height = 300};
    ui_display_list_reset(&dl);
    CHECK(ui_scene_build(&f.s, f.surface, &dl, NULL) == UI_OK);
    const ui_dl_batch *last = &dl.batches[dl.batch_count - 1]; // the child
    float right = 50 + shown(&f, 3)[0] - 50 + w - 2; // B's left + drawn width - border
    CHECK(last->kind == UI_DL_RECTS && last->clip.x == 52 && last->clip.width <= (int32_t)ceilf(right - 52) + 1);
    CHECK(last->clip.width < 40);
    f.now += 3000 * MS;
    frame(&f);
    ui_display_list_reset(&dl);
    CHECK(ui_scene_build(&f.s, f.surface, &dl, NULL) == UI_OK);
    last = &dl.batches[dl.batch_count - 1];
    CHECK(!moving(&f, 3) && last->clip.width == INT32_MAX); // at rest it overflows, unclipped
    ui_display_list_free(&dl);
    teardown(&f);
}

// ---- Both schedulers ---------------------------------------------------------------------

#define WIDTH 240u
#define HEIGHT 80u

// Three boxes in a row with layout transitions; a timer at 100 ms moves the first to the
// end, and all three slide into their new places.
typedef struct anim_logic {
    ui_handle_allocator *alloc;
    ui_handle row, boxes[3];
    bool built;
} anim_logic;

static ui_result on_events(void *user, const void *events, size_t sz, ui_cmd_writer *w) {
    anim_logic *l = user;
    ui_event_reader rd;
    ui_result r = ui_event_reader_init(&rd, events, sz);
    ui_event ev;
    while (r == UI_OK && ui_event_next(&rd, &ev) == UI_OK) {
        if (ev.type == UI_EVENT_SURFACE_RESIZED && !l->built) {
            l->built = true;
            ui_handle_alloc(l->alloc, &l->row);
            ui_cmd_create_node(w, l->row, ev.surface);
            ui_cmd_set_props(w, l->row, (ui_style_decl[]){{UI_STYLE_FLEX_GROW, ui_number(1)},
                                                          {UI_STYLE_PADDING_TOP, ui_px(20)},
                                                          {UI_STYLE_PADDING_LEFT, ui_px(15)},
                                                          {UI_STYLE_COLUMN_GAP, ui_px(10)},
                                                          {UI_STYLE_ALIGN_ITEMS, ui_keyword(UI_FLEX_START)}},
                             5);
            ui_cmd_set_color(w, l->row, 0.95f, 0.95f, 0.95f, 1);
            static const float colors[3][3] = {{0.9f, 0.2f, 0.2f}, {0.2f, 0.7f, 0.3f}, {0.2f, 0.3f, 0.9f}};
            for (int i = 0; i < 3; i++) {
                ui_handle_alloc(l->alloc, &l->boxes[i]);
                ui_cmd_create_node(w, l->boxes[i], l->row);
                ui_cmd_set_props(w, l->boxes[i],
                                 (ui_style_decl[]){{UI_STYLE_WIDTH, ui_px(60)}, {UI_STYLE_HEIGHT, ui_px(40)},
                                                   {UI_STYLE_CORNER_RADIUS, ui_px(6)},
                                                   {UI_STYLE_TRANSITION(UI_STYLE_LAYOUT), ui_transition(80, 14)}},
                                 4);
                ui_cmd_set_color(w, l->boxes[i], colors[i][0], colors[i][1], colors[i][2], 1);
            }
            ui_cmd_set_timer(w, 1, 100 * MS);
        } else if (ev.type == UI_EVENT_TIMER_FIRED) {
            ui_cmd_move_node(w, l->boxes[0], l->row, (ui_handle){0});
        }
    }
    return UI_OK;
}

// Captures the surface mid-animation and at rest.
static bool run(ui_scheduler_kind scheduler, ui_image *mid, ui_image *end) {
    anim_logic logic = {0};
    if (ui_handle_allocator_create(&logic.alloc) != UI_OK) return false;
    ui_app_desc desc = {
        .scheduler = scheduler,
        .gpu = UI_GPU_SOFTWARE,
        .surface = {.kind = UI_SURFACE_HEADLESS, .width = WIDTH, .height = HEIGHT, .scale_120 = 150},
        .logic = {.user = &logic, .on_events = on_events},
        .start_paused = true,
    };
    ui_app *app;
    ui_result r = ui_app_create(&desc, &app);
    bool ok = r == UI_OK && ui_app_frame(app) == UI_OK && ui_app_frame(app) == UI_OK;
    if (ok) {
        ui_app_clock_step(app, 100 * MS); // the timer fires; the move applies next frame
        ok = ui_app_frame(app) == UI_OK && ui_app_frame(app) == UI_OK;
    }
    for (int i = 0; ok && i < 8; i++) { // 128 ms into the slide
        ui_app_clock_step(app, 16 * MS);
        ok = ui_app_frame(app) == UI_OK;
    }
    ok = ok && ui_surface_capture(app, ui_app_main_surface(app), mid) == UI_OK;
    if (ok) {
        ui_app_clock_step(app, 3000 * MS);
        ok = ui_app_frame(app) == UI_OK && ui_app_frame(app) == UI_OK &&
             ui_surface_capture(app, ui_app_main_surface(app), end) == UI_OK;
    }
    if (ok) {
        ui_app_stats st;
        ui_app_get_stats(app, &st);
        CHECK_EQ_INT(st.commands_dropped, 0);
    }
    if (r == UI_OK) ui_app_destroy(app);
    ui_handle_allocator_destroy(logic.alloc);
    return ok;
}

// Two cards with a paragraph shrink from 300 to 140 px wide with a layout transition; one
// clips its content (the default), the other reflows its text. Captured mid-shrink.
#define FONT(name) UI_TEST_FONTS "/" name
static const char *const shrink_fonts[] = {FONT("NotoSans-VF.ttf")};

typedef struct shrink_logic {
    ui_handle_allocator *alloc;
    ui_handle cards[2];
    bool built;
} shrink_logic;

static ui_result on_shrink_events(void *user, const void *events, size_t sz, ui_cmd_writer *w) {
    shrink_logic *l = user;
    ui_event_reader rd;
    ui_result r = ui_event_reader_init(&rd, events, sz);
    ui_event ev;
    while (r == UI_OK && ui_event_next(&rd, &ev) == UI_OK) {
        if (ev.type == UI_EVENT_SURFACE_RESIZED && !l->built) {
            l->built = true;
            ui_handle col;
            ui_handle_alloc(l->alloc, &col);
            ui_cmd_create_node(w, col, ev.surface);
            ui_cmd_set_props(w, col, (ui_style_decl[]){{UI_STYLE_FLEX_GROW, ui_number(1)},
                                                       {UI_STYLE_FLEX_DIRECTION, ui_keyword(UI_FLEX_COLUMN)},
                                                       {UI_STYLE_ALIGN_ITEMS, ui_keyword(UI_FLEX_START)},
                                                       {UI_STYLE_ROW_GAP, ui_px(8)}, {UI_STYLE_PADDING_TOP, ui_px(8)},
                                                       {UI_STYLE_PADDING_LEFT, ui_px(8)},
                                                       {UI_STYLE_FONT_FAMILY, ui_string("Noto Sans")},
                                                       {UI_STYLE_FONT_SIZE, ui_px(13)},
                                                       {UI_STYLE_TEXT_COLOR, ui_rgba(0.1f, 0.1f, 0.15f, 1)}},
                             9);
            ui_cmd_set_color(w, col, 0.96f, 0.96f, 0.96f, 1);
            for (int i = 0; i < 2; i++) {
                ui_handle_alloc(l->alloc, &l->cards[i]);
                ui_cmd_create_node(w, l->cards[i], col);
                ui_cmd_set_props(w, l->cards[i],
                                 (ui_style_decl[]){{UI_STYLE_WIDTH, ui_px(300)}, {UI_STYLE_PADDING_TOP, ui_px(6)},
                                                   {UI_STYLE_PADDING_LEFT, ui_px(8)}, {UI_STYLE_PADDING_RIGHT, ui_px(8)},
                                                   {UI_STYLE_PADDING_BOTTOM, ui_px(6)}, {UI_STYLE_BORDER_WIDTH, ui_px(1)},
                                                   {UI_STYLE_BORDER_COLOR, ui_rgba(0.6f, 0.6f, 0.7f, 1)},
                                                   {UI_STYLE_CORNER_RADIUS, ui_px(6)},
                                                   {UI_STYLE_TRANSITION(UI_STYLE_LAYOUT), ui_transition(60, 15)},
                                                   {UI_STYLE_CONTENT_ANIMATION,
                                                    ui_keyword(i ? UI_CONTENT_REFLOW : UI_CONTENT_CLIP)}},
                                 10);
                ui_cmd_set_color(w, l->cards[i], 1, 1, 1, 1);
                static const char text[] = "A paragraph in a card that shrinks, wrapping as it goes.";
                ui_text_desc t = {.text = text, .length = sizeof text - 1};
                ui_cmd_set_text(w, l->cards[i], &t);
            }
            ui_cmd_set_timer(w, 1, 50 * MS);
        } else if (ev.type == UI_EVENT_TIMER_FIRED) {
            for (int i = 0; i < 2; i++) ui_cmd_set_props(w, l->cards[i], &(ui_style_decl){UI_STYLE_WIDTH, ui_px(140)}, 1);
        }
    }
    return UI_OK;
}

static bool run_shrink(ui_scheduler_kind scheduler, ui_image *mid) {
    shrink_logic logic = {0};
    if (ui_handle_allocator_create(&logic.alloc) != UI_OK) return false;
    ui_app_desc desc = {
        .scheduler = scheduler,
        .gpu = UI_GPU_SOFTWARE,
        .surface = {.kind = UI_SURFACE_HEADLESS, .width = 320, .height = 120, .scale_120 = 150},
        .logic = {.user = &logic, .on_events = on_shrink_events},
        .start_paused = true,
        .text = {.font_files = shrink_fonts, .font_file_count = 1},
    };
    ui_app *app;
    ui_result r = ui_app_create(&desc, &app);
    bool ok = r == UI_OK && ui_app_frame(app) == UI_OK && ui_app_frame(app) == UI_OK;
    if (ok) {
        ui_app_clock_step(app, 50 * MS); // the timer fires; the new width applies next frame
        ok = ui_app_frame(app) == UI_OK && ui_app_frame(app) == UI_OK;
    }
    for (int i = 0; ok && i < 6; i++) { // 96 ms into the shrink
        ui_app_clock_step(app, 16 * MS);
        ok = ui_app_frame(app) == UI_OK;
    }
    ok = ok && ui_surface_capture(app, ui_app_main_surface(app), mid) == UI_OK;
    if (r == UI_OK) ui_app_destroy(app);
    ui_handle_allocator_destroy(logic.alloc);
    return ok;
}

// Mid-shrink, the top card's text keeps its final wrapping and is cut at the box's edge;
// the bottom card's text wraps to the box as drawn.
static void test_shrink_golden(void) {
    ui_image single = {0}, threaded = {0};
    bool ok = run_shrink(UI_SCHEDULER_SINGLE, &single) && run_shrink(UI_SCHEDULER_THREADED, &threaded);
    CHECK(ok);
    if (ok) golden_check("anim_content_mid_shrink@125", &single, &threaded);
    ui_image_free(&single);
    ui_image_free(&threaded);
}

static bool same_image(const ui_image *a, const ui_image *b) {
    if (a->width != b->width || a->height != b->height) return false;
    for (uint32_t y = 0; y < a->height; y++)
        if (memcmp(a->pixels + (size_t)y * a->stride, b->pixels + (size_t)y * b->stride, 4u * a->width)) return false;
    return true;
}

static void test_schedulers(void) {
    ui_image mid[2] = {0}, end[2] = {0};
    bool ok = run(UI_SCHEDULER_SINGLE, &mid[0], &end[0]) && run(UI_SCHEDULER_THREADED, &mid[1], &end[1]);
    CHECK(ok);
    if (ok) {
        CHECK(same_image(&mid[0], &mid[1]));
        CHECK(same_image(&end[0], &end[1]));
        CHECK(!same_image(&mid[0], &end[0])); // still sliding at 128 ms
        // At rest the red box is last: (15 + 2 * 70 + 30) * 1.25 = 231 px, mid-height.
        const uint8_t *p = end[0].pixels + (size_t)(40 * 1.25) * end[0].stride + 4u * 231;
        CHECK(p[0] > 200 && p[1] < 80 && p[2] < 80);
    }
    for (int i = 0; i < 2; i++) {
        ui_image_free(&mid[i]);
        ui_image_free(&end[i]);
    }
}

int main(void) {
    RUN(test_moves);
    RUN(test_retarget);
    RUN(test_children_follow);
    RUN(test_reparent);
    RUN(test_transition_removed);
    RUN(test_shown_again);
    RUN(test_wire);
    RUN(test_content_clip);
    RUN(test_schedulers);
    RUN(test_shrink_golden);
    return test_failures != 0;
}
