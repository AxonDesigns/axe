// Golden images of the paint properties (borders, per-side borders and corners, opacity
// groups, clipping) and of a flex layout with text, at scales 1.0, 1.25 and 1.5 under both
// schedulers (tests/golden.h has the rules), plus checks that need no golden:
//   - a group's overlapping children composite once at the group's opacity (CSS group
//     opacity), not each at that opacity;
//   - a box at rest snaps to whole pixels, a moving one (and its subtree) doesn't.
#include <math.h>

#include "core/scene.h"
#include "golden.h"

#define WIDTH 320u
#define HEIGHT 160u
#define FONT(name) UI_TEST_FONTS "/" name

static const char *const font_files[] = {FONT("NotoSans-VF.ttf")};
static const char *const families[] = {"Noto Sans"};

typedef struct builder {
    ui_cmd_writer *w;
    ui_handle_allocator *alloc;
} builder;

static ui_handle node(builder *b, ui_handle parent, float x, float y, float w, float h) {
    ui_handle n;
    ui_handle_alloc(b->alloc, &n);
    ui_cmd_create_node(b->w, n, parent);
    ui_cmd_set_rect(b->w, n, x, y, w, h);
    return n;
}

static ui_handle box(builder *b, ui_handle parent, float x, float y, float w, float h, float r,
                     float g, float bl, float a) {
    ui_handle n = node(b, parent, x, y, w, h);
    ui_cmd_set_color(b->w, n, r, g, bl, a);
    return n;
}

static void props(builder *b, ui_handle n, const ui_style_decl *d, uint32_t count) {
    CHECK(ui_cmd_set_props(b->w, n, d, count) == UI_OK);
}

static void text(builder *b, ui_handle n, const char *s, float size) {
    ui_text_desc d = {.set = UI_TEXT_SET_ALL, .text = s, .length = (uint32_t)strlen(s),
                      .style = {.family = 0, .weight = 600, .size = size, .color = {0.1f, 0.1f, 0.12f, 1}},
                      .families = families, .family_count = 1};
    CHECK(ui_cmd_set_text(b->w, n, &d) == UI_OK);
}

static void borders(builder *b, ui_handle root) {
    ui_handle a = box(b, root, 16, 16, 80, 50, 0.75f, 0.85f, 0.95f, 1);
    props(b, a, (ui_style_decl[]){{UI_STYLE_BORDER_WIDTH, ui_px(1)},
                                  {UI_STYLE_BORDER_COLOR, ui_rgba(0.1f, 0.1f, 0.2f, 1)}}, 2);
    ui_handle c = box(b, root, 112, 16, 80, 50, 1, 0.85f, 0.3f, 1);
    props(b, c, (ui_style_decl[]){{UI_STYLE_BORDER_WIDTH, ui_px(4)},
                                  {UI_STYLE_BORDER_COLOR, ui_rgba(0.1f, 0.3f, 0.8f, 1)},
                                  {UI_STYLE_CORNER_RADIUS, ui_px(12)}}, 3);
    ui_handle pill = node(b, root, 208, 16, 96, 50); // a border and no background
    props(b, pill, (ui_style_decl[]){{UI_STYLE_BORDER_WIDTH, ui_px(2)},
                                     {UI_STYLE_BORDER_COLOR, ui_rgba(0.85f, 0.15f, 0.1f, 1)},
                                     {UI_STYLE_CORNER_RADIUS, ui_px(25)}}, 3);
    ui_handle soft = box(b, root, 16, 90, 80, 50, 0.2f, 0.7f, 0.3f, 1); // the background shows
    props(b, soft, (ui_style_decl[]){{UI_STYLE_BORDER_WIDTH, ui_px(6)}, // under the border
                                     {UI_STYLE_BORDER_COLOR, ui_rgba(0, 0, 0, 0.4f)},
                                     {UI_STYLE_CORNER_RADIUS, ui_px(8)}}, 3);
    ui_handle thick = box(b, root, 112, 90, 40, 40, 1, 1, 1, 1); // thicker than half: filled
    props(b, thick, (ui_style_decl[]){{UI_STYLE_BORDER_WIDTH, ui_px(30)},
                                      {UI_STYLE_BORDER_COLOR, ui_rgba(0.5f, 0.2f, 0.6f, 1)},
                                      {UI_STYLE_CORNER_RADIUS, ui_px(6)}}, 3);
    // Through a style and a token, as apps will write it.
    CHECK(ui_cmd_set_token(b->w, 1, ui_rgba(0.95f, 0.5f, 0.1f, 1)) == UI_OK);
    ui_style_decl outlined[] = {{UI_STYLE_BORDER_WIDTH, ui_px(3)},
                                {UI_STYLE_BORDER_COLOR, ui_token_ref(1)},
                                {UI_STYLE_CORNER_RADIUS, ui_px(4)},
                                {UI_STYLE_BACKGROUND_COLOR, ui_rgba(1, 0.97f, 0.9f, 1)}};
    ui_style_block_desc block = {.decls = outlined, .count = 4};
    CHECK(ui_cmd_define_style(b->w, 7, &block, 1) == UI_OK);
    ui_handle styled = node(b, root, 168, 90, 136, 50);
    CHECK(ui_cmd_set_styles(b->w, styled, (uint32_t[]){7}, 1) == UI_OK);
    text(b, styled, "Styled", 18);
}

static void opacity(builder *b, ui_handle root) {
    // Overlapping children in a group at 0.5: blue covers red inside the group.
    ui_handle group = node(b, root, 16, 16, 140, 128);
    props(b, group, (ui_style_decl[]){{UI_STYLE_OPACITY, ui_number(0.5f)}}, 1);
    box(b, group, 10, 10, 80, 80, 1, 0, 0, 1);
    box(b, group, 50, 30, 80, 80, 0, 0, 1, 1);
    ui_handle label = node(b, group, 10, 100, 130, 24);
    text(b, label, "Group 50%", 16);
    // A group inside a group, with a background and text.
    ui_handle outer = box(b, root, 172, 16, 132, 100, 0.2f, 0.6f, 0.3f, 1);
    props(b, outer, (ui_style_decl[]){{UI_STYLE_OPACITY, ui_number(0.6f)},
                                      {UI_STYLE_CORNER_RADIUS, ui_px(10)}}, 2);
    ui_handle inner = box(b, outer, 20, 20, 92, 60, 1, 0.6f, 0.1f, 1);
    props(b, inner, (ui_style_decl[]){{UI_STYLE_OPACITY, ui_number(0.5f)}}, 1);
    text(b, inner, "Nested", 18);
    // A lone background: its alpha is multiplied, no layer.
    ui_handle lone = box(b, root, 172, 124, 132, 20, 0, 0, 0, 1);
    props(b, lone, (ui_style_decl[]){{UI_STYLE_OPACITY, ui_number(0.4f)}}, 1);
}

static void clipping(builder *b, ui_handle root) {
    ui_handle c1 = box(b, root, 16, 16, 120, 60, 0.85f, 0.85f, 0.88f, 1);
    ui_cmd_set_clip(b->w, c1, true);
    box(b, c1, -20, 36, 200, 16, 0.1f, 0.2f, 0.6f, 1);
    ui_handle line = node(b, c1, 4, 4, 300, 30);
    text(b, line, "Clipped text runs past the edge", 16);
    // A clip inside a layer.
    ui_handle layer = node(b, root, 160, 16, 144, 128);
    props(b, layer, (ui_style_decl[]){{UI_STYLE_OPACITY, ui_number(0.7f)}}, 1);
    ui_handle c2 = box(b, layer, 10, 10, 100, 60, 1, 0.75f, 0.8f, 1);
    ui_cmd_set_clip(b->w, c2, true);
    box(b, c2, 40, 30, 120, 60, 0.1f, 0.6f, 0.6f, 1);
    // Nested clips: only the intersection shows.
    ui_handle c3 = box(b, root, 16, 90, 120, 56, 0.95f, 0.95f, 0.9f, 1);
    props(b, c3, (ui_style_decl[]){{UI_STYLE_CLIP, ui_keyword(UI_CLIP_RECT)}}, 1);
    ui_handle c4 = node(b, c3, 60, -10, 100, 40);
    ui_cmd_set_clip(b->w, c4, true);
    box(b, c4, -100, 0, 300, 300, 0.5f, 0.2f, 0.7f, 1);
}

// A flex layout with text, built only through styles: a card (a column of a title, a
// wrapping paragraph and a footer row with a label and a padded button at either end) next
// to a growing column: rows with a dot and text centered on it, a row aligned by baseline,
// and an item aligned to the end.
static ui_handle flex(builder *b, ui_handle parent, const ui_style_decl *d, uint32_t count) {
    ui_handle n;
    ui_handle_alloc(b->alloc, &n);
    ui_cmd_create_node(b->w, n, parent);
    if (count) props(b, n, d, count);
    return n;
}

#define FLEX(b, parent, ...) \
    flex(b, parent, (ui_style_decl[]){__VA_ARGS__}, sizeof((ui_style_decl[]){__VA_ARGS__}) / sizeof(ui_style_decl))

static void flex_text(builder *b, ui_handle root) {
    props(b, root, (ui_style_decl[]){{UI_STYLE_PADDING_TOP, ui_px(10)}, {UI_STYLE_PADDING_LEFT, ui_px(10)},
                                     {UI_STYLE_PADDING_RIGHT, ui_px(10)}, {UI_STYLE_COLUMN_GAP, ui_px(10)},
                                     {UI_STYLE_ALIGN_ITEMS, ui_keyword(UI_FLEX_START)}}, 5);
    ui_handle card = FLEX(b, root, {UI_STYLE_WIDTH, ui_px(150)}, {UI_STYLE_FLEX_DIRECTION, ui_keyword(UI_FLEX_COLUMN)},
                          {UI_STYLE_PADDING_TOP, ui_px(10)}, {UI_STYLE_PADDING_RIGHT, ui_px(10)},
                          {UI_STYLE_PADDING_BOTTOM, ui_px(10)}, {UI_STYLE_PADDING_LEFT, ui_px(10)},
                          {UI_STYLE_ROW_GAP, ui_px(6)}, {UI_STYLE_BORDER_WIDTH, ui_px(1)},
                          {UI_STYLE_BORDER_COLOR, ui_rgba(0.8f, 0.82f, 0.86f, 1)},
                          {UI_STYLE_CORNER_RADIUS, ui_px(8)}, {UI_STYLE_BACKGROUND_COLOR, ui_rgba(1, 1, 1, 1)});
    text(b, FLEX(b, card, {UI_STYLE_MAX_LINES, ui_number(1)}), "Flex card", 16);
    ui_handle body = FLEX(b, card, {UI_STYLE_LINE_HEIGHT, ui_number(1.3f)});
    text(b, body, "A paragraph wraps to the card's width.", 11);
    ui_handle footer = FLEX(b, card, {UI_STYLE_JUSTIFY_CONTENT, ui_keyword(UI_FLEX_SPACE_BETWEEN)},
                            {UI_STYLE_ALIGN_ITEMS, ui_keyword(UI_FLEX_CENTER)});
    text(b, FLEX(b, footer, {UI_STYLE_TEXT_COLOR, ui_rgba(0.45f, 0.47f, 0.5f, 1)}), "#12", 11);
    text(b, FLEX(b, footer, {UI_STYLE_PADDING_TOP, ui_px(3)}, {UI_STYLE_PADDING_BOTTOM, ui_px(3)},
                 {UI_STYLE_PADDING_LEFT, ui_px(8)}, {UI_STYLE_PADDING_RIGHT, ui_px(8)},
                 {UI_STYLE_CORNER_RADIUS, ui_px(5)}, {UI_STYLE_BACKGROUND_COLOR, ui_rgba(0.2f, 0.45f, 0.95f, 1)},
                 {UI_STYLE_TEXT_COLOR, ui_rgba(1, 1, 1, 1)}),
         "Open", 11);
    ui_handle side = FLEX(b, root, {UI_STYLE_FLEX_GROW, ui_number(1)},
                          {UI_STYLE_FLEX_DIRECTION, ui_keyword(UI_FLEX_COLUMN)}, {UI_STYLE_ROW_GAP, ui_px(6)});
    static const char *const items[] = {"Centered on its dot", "Grows to fill"};
    static const float dots[][3] = {{0.9f, 0.3f, 0.2f}, {0.2f, 0.7f, 0.4f}};
    for (int i = 0; i < 2; i++) {
        ui_handle row = FLEX(b, side, {UI_STYLE_ALIGN_ITEMS, ui_keyword(UI_FLEX_CENTER)}, {UI_STYLE_COLUMN_GAP, ui_px(6)},
                             {UI_STYLE_BACKGROUND_COLOR, ui_rgba(0.9f, 0.92f, 0.95f, 1)},
                             {UI_STYLE_PADDING_TOP, ui_px(4)}, {UI_STYLE_PADDING_BOTTOM, ui_px(4)},
                             {UI_STYLE_PADDING_LEFT, ui_px(6)}, {UI_STYLE_CORNER_RADIUS, ui_px(4)});
        FLEX(b, row, {UI_STYLE_WIDTH, ui_px(10)}, {UI_STYLE_HEIGHT, ui_px(10)}, {UI_STYLE_CORNER_RADIUS, ui_px(5)},
             {UI_STYLE_BACKGROUND_COLOR, ui_rgba(dots[i][0], dots[i][1], dots[i][2], 1)});
        text(b, FLEX(b, row, {UI_STYLE_FLEX_GROW, ui_number(1)}), items[i], 12);
    }
    ui_handle base = FLEX(b, side, {UI_STYLE_ALIGN_ITEMS, ui_keyword(UI_FLEX_BASELINE)}, {UI_STYLE_COLUMN_GAP, ui_px(6)},
                          {UI_STYLE_BORDER_WIDTH, ui_px(1)}, {UI_STYLE_BORDER_COLOR, ui_rgba(0.85f, 0.6f, 0.2f, 1)});
    text(b, FLEX(b, base, {UI_STYLE_PADDING_TOP, ui_px(6)}), "Big", 24);
    text(b, flex(b, base, NULL, 0), "baseline", 12);
    ui_handle end = FLEX(b, side, {UI_STYLE_ALIGN_SELF, ui_keyword(UI_FLEX_END)},
                         {UI_STYLE_BACKGROUND_COLOR, ui_rgba(0.95f, 0.85f, 0.95f, 1)},
                         {UI_STYLE_PADDING_LEFT, ui_px(6)}, {UI_STYLE_PADDING_RIGHT, ui_px(6)});
    text(b, end, "End", 12);
}

// Per-side borders and per-corner radii (docs/styles.md, "Borders"): unequal widths with
// large radii (elliptical inner corners), transparent sides, radii too large for their
// sides (scaled down together), four colors meeting on the corner diagonals, and logical
// sides in a right-to-left node.
static void border_sides(builder *b, ui_handle root) {
    ui_handle a = box(b, root, 12, 12, 90, 60, 0.95f, 0.92f, 0.85f, 1);
    props(b, a, (ui_style_decl[]){{UI_STYLE_BORDER_TOP_WIDTH, ui_px(2)}, {UI_STYLE_BORDER_RIGHT_WIDTH, ui_px(12)},
                                  {UI_STYLE_BORDER_BOTTOM_WIDTH, ui_px(6)}, {UI_STYLE_BORDER_LEFT_WIDTH, ui_px(20)},
                                  {UI_STYLE_BORDER_COLOR, ui_rgba(0.15f, 0.35f, 0.75f, 1)},
                                  {UI_STYLE_CORNER_RADIUS, ui_px(24)}}, 6);
    ui_handle c = box(b, root, 116, 12, 90, 60, 1, 1, 1, 1);
    props(b, c, (ui_style_decl[]){{UI_STYLE_BORDER_WIDTH, ui_px(10)},
                                  {UI_STYLE_BORDER_TOP_COLOR, ui_rgba(0.9f, 0.2f, 0.2f, 1)},
                                  {UI_STYLE_BORDER_RIGHT_COLOR, ui_rgba(0.2f, 0.7f, 0.3f, 1)},
                                  {UI_STYLE_BORDER_BOTTOM_COLOR, ui_rgba(0.2f, 0.3f, 0.9f, 1)},
                                  {UI_STYLE_BORDER_LEFT_COLOR, ui_rgba(0.9f, 0.7f, 0.1f, 1)}}, 5);
    ui_handle d = box(b, root, 220, 12, 90, 60, 0.85f, 0.95f, 0.9f, 1); // transparent sides
    props(b, d, (ui_style_decl[]){{UI_STYLE_BORDER_WIDTH, ui_px(6)}, {UI_STYLE_BORDER_COLOR, ui_rgba(0.3f, 0.2f, 0.5f, 1)},
                                  {UI_STYLE_BORDER_RIGHT_COLOR, ui_rgba(0, 0, 0, 0)},
                                  {UI_STYLE_BORDER_LEFT_COLOR, ui_rgba(0, 0, 0, 0)},
                                  {UI_STYLE_CORNER_RADIUS_TOP_LEFT, ui_px(18)},
                                  {UI_STYLE_CORNER_RADIUS_BOTTOM_RIGHT, ui_px(18)}}, 6);
    ui_handle e = box(b, root, 12, 88, 90, 60, 0.9f, 0.85f, 0.95f, 1); // radii larger than the sides
    props(b, e, (ui_style_decl[]){{UI_STYLE_CORNER_RADIUS_TOP_LEFT, ui_px(80)}, {UI_STYLE_CORNER_RADIUS_TOP_RIGHT, ui_px(40)},
                                  {UI_STYLE_CORNER_RADIUS_BOTTOM_RIGHT, ui_px(10)},
                                  {UI_STYLE_BORDER_WIDTH, ui_px(3)}, {UI_STYLE_BORDER_COLOR, ui_rgba(0.4f, 0.1f, 0.4f, 1)}}, 5);
    ui_handle f = box(b, root, 116, 88, 90, 60, 1, 1, 1, 1); // one thick side and a large radius
    props(b, f, (ui_style_decl[]){{UI_STYLE_BORDER_LEFT_WIDTH, ui_px(16)},
                                  {UI_STYLE_BORDER_LEFT_COLOR, ui_rgba(0.85f, 0.35f, 0.1f, 1)},
                                  {UI_STYLE_BORDER_TOP_WIDTH, ui_px(1)},
                                  {UI_STYLE_BORDER_TOP_COLOR, ui_rgba(0.5f, 0.5f, 0.5f, 1)},
                                  {UI_STYLE_CORNER_RADIUS_TOP_LEFT, ui_px(30)}}, 5);
    ui_handle g = box(b, root, 220, 88, 90, 60, 0.95f, 0.95f, 1, 1); // logical sides, right to left
    props(b, g, (ui_style_decl[]){{UI_STYLE_DIRECTION, ui_keyword(UI_DIRECTION_RTL)},
                                  {UI_STYLE_BORDER_START_WIDTH, ui_px(14)},
                                  {UI_STYLE_BORDER_START_COLOR, ui_rgba(0.1f, 0.5f, 0.6f, 1)},
                                  {UI_STYLE_BORDER_END_WIDTH, ui_px(2)},
                                  {UI_STYLE_BORDER_END_COLOR, ui_rgba(0.6f, 0.1f, 0.3f, 1)},
                                  {UI_STYLE_CORNER_RADIUS_TOP_START, ui_px(22)}}, 6);
}

typedef struct paint_case {
    const char *name;
    void (*build)(builder *b, ui_handle root);
} paint_case;

static const paint_case cases[] = {
    {"paint_borders", borders},
    {"paint_opacity", opacity},
    {"paint_clipping", clipping},
    {"paint_flex_text", flex_text},
    {"paint_border_sides", border_sides},
};

typedef struct paint_logic {
    const paint_case *c;
    ui_handle_allocator *alloc;
    bool built;
} paint_logic;

static ui_result on_events(void *user, const void *events, size_t size, ui_cmd_writer *cmds) {
    paint_logic *l = user;
    ui_event_reader rd;
    ui_result r = ui_event_reader_init(&rd, events, size);
    ui_event ev;
    while (r == UI_OK && ui_event_next(&rd, &ev) == UI_OK) {
        if (ev.type != UI_EVENT_SURFACE_RESIZED || l->built) continue;
        l->built = true;
        builder b = {cmds, l->alloc};
        ui_handle root = box(&b, ev.surface, 0, 0, WIDTH, HEIGHT, 0.97f, 0.97f, 0.97f, 1);
        l->c->build(&b, root);
    }
    return UI_OK;
}

static bool render(const paint_case *c, uint32_t scale_120, ui_scheduler_kind scheduler,
                   ui_image *out) {
    paint_logic logic = {.c = c};
    if (ui_handle_allocator_create(&logic.alloc) != UI_OK) return false;
    ui_app_desc desc = {
        .scheduler = scheduler,
        .gpu = UI_GPU_SOFTWARE,
        .surface = {.kind = UI_SURFACE_HEADLESS, .width = WIDTH, .height = HEIGHT,
                    .scale_120 = scale_120},
        .logic = {.user = &logic, .on_events = on_events},
        .start_paused = true,
        .text = {.font_files = font_files, .font_file_count = 1},
    };
    ui_app *app;
    ui_result r = ui_app_create(&desc, &app);
    bool ok = r == UI_OK && ui_app_frame(app) == UI_OK && ui_app_frame(app) == UI_OK &&
              ui_surface_capture(app, ui_app_main_surface(app), out) == UI_OK;
    if (ok) {
        ui_app_stats st;
        ui_app_get_stats(app, &st);
        CHECK_EQ_INT(st.commands_dropped, 0);
    }
    if (r == UI_OK) ui_app_destroy(app);
    ui_handle_allocator_destroy(logic.alloc);
    return ok;
}

static const uint8_t *pixel(const ui_image *img, double x, double y) {
    return img->pixels + (size_t)y * img->stride + 4u * (size_t)x;
}

static void test_goldens(void) {
    static const uint32_t scales[] = {120, 150, 180};
    for (size_t i = 0; i < UI_ARRAY_LEN(cases); i++) {
        for (size_t k = 0; k < UI_ARRAY_LEN(scales); k++) {
            ui_image single = {0}, threaded = {0};
            bool ok = render(&cases[i], scales[k], UI_SCHEDULER_SINGLE, &single) &&
                      render(&cases[i], scales[k], UI_SCHEDULER_THREADED, &threaded);
            CHECK(ok);
            if (ok) {
                char name[128];
                snprintf(name, sizeof name, "%s@%u", cases[i].name, scales[k] * 100 / 120);
                golden_check(name, &single, &threaded);
            }
            if (ok && cases[i].build == opacity) {
                // Inside the overlap blue is on top: the group shows blue at 50% over the
                // light background, with no red. Blending each child at 50% instead would
                // leave red visible (R well above G).
                double s = scales[k] / 120.0;
                const uint8_t *p = pixel(&single, 86 * s, 76 * s);
                CHECK(abs(p[0] - p[1]) <= 3 && p[2] > 245 && p[0] > 170 && p[0] < 200);
            }
            ui_image_free(&single);
            ui_image_free(&threaded);
        }
    }
}

// A slow spring draws at subpixel positions while it moves, and snaps once it settles; a
// child of a moving node moves with it, unsnapped.
static void test_snapping(void) {
    ui_logger quiet = {UI_LOG_ERROR};
    ui_scene s;
    ui_scene_init(&s, &quiet);
    ui_handle surface = {(uint32_t)UI_HANDLE_SURFACE << UI_HANDLE_SLOT_BITS, 1};
    ui_scene_add_root(&s, surface);
    ui_cmd_writer *w;
    ui_cmd_writer_create(&w);
    ui_handle a = {1, 1}, child = {2, 1};
    ui_cmd_create_node(w, a, surface);
    ui_cmd_set_rect(w, a, 10.3f, 4, 20.4f, 10);
    ui_cmd_set_color(w, a, 1, 0, 0, 1);
    ui_cmd_create_node(w, child, a);
    ui_cmd_set_rect(w, child, 3.3f, 1, 5, 5);
    ui_cmd_set_color(w, child, 0, 0, 1, 1);
    uint8_t *bytes;
    size_t size;
    ui_cmd_writer_take(w, &bytes, &size);
    CHECK(ui_scene_apply(&s, bytes, size, 0) == UI_OK);
    free(bytes);
    ui_scene_resolve(&s, 0);
    ui_display_list dl = {.scale_120 = 150, .width = 400, .height = 200};
    ui_display_list_reset(&dl);
    ui_scene_layout(&s, surface, 320, 160, 1.25f, NULL, 0);
    CHECK(ui_scene_build(&s, surface, &dl, NULL) == UI_OK);
    // At rest: origin and size each round (12.875 -> 13, 25.5 -> 26; child 17 -> 17).
    CHECK_EQ_INT(dl.rect_count, 2);
    CHECK_NEAR(dl.rects[0].x, 13, 0);
    CHECK_NEAR(dl.rects[0].width, 26, 0);
    CHECK_NEAR(dl.rects[1].x, 17, 0);

    float target[4] = {50.3f, 4, 20.4f, 10};
    ui_cmd_start_spring(w, a, UI_PROP_RECT, target, 20, 9); // slow
    ui_cmd_writer_take(w, &bytes, &size);
    CHECK(ui_scene_apply(&s, bytes, size, 0) == UI_OK);
    free(bytes);
    bool fractional = false;
    for (uint64_t t = 16000000; t < 400000000; t += 16000000) {
        ui_scene_animate(&s, t);
        ui_display_list_reset(&dl);
        ui_scene_layout(&s, surface, 320, 160, 1.25f, NULL, t);
        ui_scene_build(&s, surface, &dl, NULL);
        // The parent is its resting pixel (63) plus what it has left to travel, exactly, so
        // it glides and lands without a snap; the child keeps its resting offset (67 - 63).
        float left = s.nodes[ui_handle_slot(a)].rect[0] - 50.3f;
        CHECK_NEAR(dl.rects[0].x, 63 + left * 1.25f, 1e-3);
        CHECK_NEAR(dl.rects[1].x - dl.rects[0].x, 4, 1e-3);
        fractional |= dl.rects[0].x != floorf(dl.rects[0].x);
    }
    CHECK(fractional);
    ui_scene_animate(&s, 20000000000ull); // long settled
    ui_display_list_reset(&dl);
    ui_scene_layout(&s, surface, 320, 160, 1.25f, NULL, 20000000000ull);
    ui_scene_build(&s, surface, &dl, NULL);
    CHECK_NEAR(dl.rects[0].x, 63, 0); // 50.3 * 1.25 = 62.875
    CHECK_NEAR(dl.rects[1].x, 67, 0); // 53.6 * 1.25 = 67
    ui_display_list_free(&dl);
    ui_cmd_writer_destroy(w);
    ui_scene_destroy(&s);
}

// Layer bounds cover what is drawn inside, within the surface; a layer with nothing visible
// is empty. Extents can be infinite or huge, which must not reach an integer conversion.
static void test_layer_bounds(void) {
    ui_logger quiet = {UI_LOG_ERROR};
    ui_scene s;
    ui_scene_init(&s, &quiet);
    ui_handle surface = {(uint32_t)UI_HANDLE_SURFACE << UI_HANDLE_SLOT_BITS, 1};
    ui_scene_add_root(&s, surface);
    ui_cmd_writer *w;
    ui_cmd_writer_create(&w);
    ui_handle empty = {1, 1}, group = {2, 1}, far = {3, 1}, near = {4, 1};
    ui_style_decl half = {UI_STYLE_OPACITY, ui_number(0.5f)};
    ui_cmd_create_node(w, empty, surface); // a group with nothing to draw
    ui_cmd_set_props(w, empty, &half, 1);
    ui_cmd_create_node(w, far, empty);
    ui_cmd_create_node(w, group, surface);
    ui_cmd_set_rect(w, group, 10, 10, 20, 20);
    ui_cmd_set_props(w, group, &half, 1);
    ui_cmd_create_node(w, near, group);
    ui_cmd_set_rect(w, near, -1.5e38f, 5, 3e38f, 10); // reaches past the surface on both sides
    ui_cmd_set_color(w, near, 1, 0, 0, 1);
    uint8_t *bytes;
    size_t size;
    ui_cmd_writer_take(w, &bytes, &size);
    CHECK(ui_scene_apply(&s, bytes, size, 0) == UI_OK);
    free(bytes);
    ui_scene_resolve(&s, 0);
    ui_display_list dl = {.scale_120 = 120, .width = 100, .height = 80};
    ui_display_list_reset(&dl);
    ui_scene_layout(&s, surface, 100, 80, 1, NULL, 0);
    CHECK(ui_scene_build(&s, surface, &dl, NULL) == UI_OK);
    CHECK_EQ_INT(dl.layer_count, 2);
    if (dl.layer_count == 2) {
        CHECK(dl.layers[0].width == 0 || dl.layers[0].height == 0);
        CHECK_EQ_INT(dl.layers[1].x, 0);
        CHECK_EQ_INT(dl.layers[1].width, 100);
        CHECK_EQ_INT(dl.layers[1].y, 14); // 15 minus the anti-aliasing margin
        CHECK_EQ_INT(dl.layers[1].height, 12);
    }
    ui_display_list_free(&dl);
    ui_cmd_writer_destroy(w);
    ui_scene_destroy(&s);
}

int main(void) {
    RUN(test_layer_bounds);
    RUN(test_snapping);
    RUN(test_goldens);
    return test_failures != 0;
}
