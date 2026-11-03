// layout_cards: a wrapping grid of cards laid out by flexbox. Every 2.5 s a card is inserted,
// moved or removed, and the cards slide to their new places (layout animations); every 4 s
// the theme switches between light and dark, and every color springs to its new value
// (theme tokens with color transitions). Resizing the window reflows the grid, animated too.
//
//   layout_cards [--no-shuffle] [--no-theme]   window
//   layout_cards --headless [--out f.ppm] [--scale N] [--at S] [--dark]
//                                               offscreen at scale N/120: steps a paused
//                                               clock to S seconds, then writes a PPM
//
// Environment: UI_SCHEDULER=threaded|single, UI_GPU=software, UI_TRACE=1, UI_LOG=debug.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/ui.h"

#define FONT(name) AXE_SOURCE_DIR "/tests/fonts/" name
#define MAX_CARDS 24
#define SHUFFLE_TIMER 1
#define THEME_TIMER 2
#define SHUFFLE_NS 2500000000ull
#define THEME_NS 4000000000ull

static const char *const font_files[] = {FONT("NotoSans-VF.ttf")};
static const char *const families[] = {"Noto Sans, sans-serif"};

static const char *const PARAGRAPHS[] = {
    "Cards wrap onto as many rows as the window allows.",
    "Each card is a flex column: a title, this paragraph, and a footer row whose label and "
    "button sit at either end.",
    "When a card is inserted or removed, the others slide to their new places.",
    "The theme is a handful of tokens. Switching it changes one value per token, and every "
    "color that refers to one springs to its new value.",
    "Text is shaped on worker threads before its frame.",
    "Resize the window: the grid reflows, and the cards animate there too, keeping their "
    "velocity when the target moves again.",
};

// Theme tokens and the styles that use them.
enum { T_PAGE = 1, T_CARD, T_BORDER, T_TITLE, T_BODY, T_MUTED, T_ACCENT, T_ACCENT_TEXT, T_COUNT };
enum { S_PAGE = 1, S_CARD, S_TITLE, S_BODY, S_FOOTER, S_LABEL, S_BUTTON };

static const float THEMES[2][T_COUNT][4] = {
    {{0}, {0.93f, 0.94f, 0.96f, 1}, {1, 1, 1, 1}, {0.84f, 0.86f, 0.9f, 1}, {0.1f, 0.11f, 0.14f, 1},
     {0.3f, 0.32f, 0.36f, 1}, {0.5f, 0.52f, 0.56f, 1}, {0.2f, 0.45f, 0.95f, 1}, {1, 1, 1, 1}},
    {{0}, {0.07f, 0.08f, 0.1f, 1}, {0.13f, 0.14f, 0.17f, 1}, {0.24f, 0.26f, 0.31f, 1},
     {0.93f, 0.94f, 0.96f, 1}, {0.72f, 0.74f, 0.78f, 1}, {0.55f, 0.57f, 0.62f, 1},
     {0.38f, 0.62f, 1, 1}, {0.05f, 0.06f, 0.08f, 1}},
};

typedef struct card {
    ui_handle node, title, body, footer, label, button;
    uint32_t id;
} card;

typedef struct demo {
    ui_handle_allocator *handles;
    ui_handle surface, page;
    card cards[MAX_CARDS]; // in display order
    uint32_t count, next_id, step;
    uint32_t seed;
    bool dark, shuffle, theme, armed;
} demo;

static uint32_t next_random(demo *d) {
    d->seed = d->seed * 1664525u + 1013904223u;
    return d->seed >> 8;
}

static ui_result set_theme(demo *d, ui_cmd_writer *w) {
    ui_result r = UI_OK;
    for (int t = T_PAGE; t < T_COUNT && r == UI_OK; t++) {
        const float *c = THEMES[d->dark][t];
        r = ui_cmd_set_token(w, (uint32_t)t, ui_rgba(c[0], c[1], c[2], c[3]));
    }
    return r;
}

#define DECLS(...) ((ui_style_decl[]){__VA_ARGS__}), (sizeof((ui_style_decl[]){__VA_ARGS__}) / sizeof(ui_style_decl))

static ui_result define(ui_cmd_writer *w, uint32_t id, const ui_style_decl *decls, uint32_t count) {
    return ui_cmd_define_style(w, id, &(ui_style_block_desc){0, decls, count}, 1);
}

static ui_result define_styles(ui_cmd_writer *w) {
    const ui_value color_spring = ui_transition(90, 19);
    ui_result r = define(w, S_PAGE, DECLS({UI_STYLE_FLEX_GROW, ui_number(1)},
                                          {UI_STYLE_FLEX_WRAP, ui_keyword(UI_FLEX_WRAP)},
                                          {UI_STYLE_ALIGN_ITEMS, ui_keyword(UI_FLEX_START)},
                                          {UI_STYLE_ALIGN_CONTENT, ui_keyword(UI_FLEX_START)},
                                          {UI_STYLE_PADDING_TOP, ui_px(20)}, {UI_STYLE_PADDING_RIGHT, ui_px(20)},
                                          {UI_STYLE_PADDING_BOTTOM, ui_px(20)}, {UI_STYLE_PADDING_LEFT, ui_px(20)},
                                          {UI_STYLE_ROW_GAP, ui_px(14)}, {UI_STYLE_COLUMN_GAP, ui_px(14)},
                                          {UI_STYLE_BACKGROUND_COLOR, ui_token_ref(T_PAGE)},
                                          {UI_STYLE_TRANSITION(UI_STYLE_BACKGROUND_COLOR), color_spring}));
    if (r == UI_OK)
        r = define(w, S_CARD, DECLS({UI_STYLE_WIDTH, ui_px(208)},
                                    {UI_STYLE_FLEX_DIRECTION, ui_keyword(UI_FLEX_COLUMN)},
                                    {UI_STYLE_PADDING_TOP, ui_px(14)}, {UI_STYLE_PADDING_RIGHT, ui_px(14)},
                                    {UI_STYLE_PADDING_BOTTOM, ui_px(12)}, {UI_STYLE_PADDING_LEFT, ui_px(14)},
                                    {UI_STYLE_ROW_GAP, ui_px(8)}, {UI_STYLE_BORDER_WIDTH, ui_px(1)},
                                    {UI_STYLE_CORNER_RADIUS, ui_px(10)},
                                    {UI_STYLE_BACKGROUND_COLOR, ui_token_ref(T_CARD)},
                                    {UI_STYLE_BORDER_COLOR, ui_token_ref(T_BORDER)},
                                    {UI_STYLE_TRANSITION(UI_STYLE_BACKGROUND_COLOR), color_spring},
                                    {UI_STYLE_TRANSITION(UI_STYLE_BORDER_COLOR), color_spring},
                                    {UI_STYLE_TRANSITION(UI_STYLE_LAYOUT), ui_transition(170, 24)}));
    if (r == UI_OK)
        r = define(w, S_TITLE, DECLS({UI_STYLE_TEXT_COLOR, ui_token_ref(T_TITLE)},
                                     {UI_STYLE_TRANSITION(UI_STYLE_TEXT_COLOR), color_spring}));
    if (r == UI_OK)
        r = define(w, S_BODY, DECLS({UI_STYLE_TEXT_COLOR, ui_token_ref(T_BODY)}, {UI_STYLE_LINE_HEIGHT, ui_number(1.4f)},
                                    {UI_STYLE_TRANSITION(UI_STYLE_TEXT_COLOR), color_spring}));
    if (r == UI_OK)
        r = define(w, S_FOOTER, DECLS({UI_STYLE_JUSTIFY_CONTENT, ui_keyword(UI_FLEX_SPACE_BETWEEN)},
                                      {UI_STYLE_ALIGN_ITEMS, ui_keyword(UI_FLEX_CENTER)},
                                      {UI_STYLE_MARGIN_TOP, ui_px(4)}));
    if (r == UI_OK)
        r = define(w, S_LABEL, DECLS({UI_STYLE_TEXT_COLOR, ui_token_ref(T_MUTED)},
                                     {UI_STYLE_TRANSITION(UI_STYLE_TEXT_COLOR), color_spring}));
    if (r == UI_OK)
        r = define(w, S_BUTTON, DECLS({UI_STYLE_PADDING_TOP, ui_px(4)}, {UI_STYLE_PADDING_RIGHT, ui_px(12)},
                                      {UI_STYLE_PADDING_BOTTOM, ui_px(4)}, {UI_STYLE_PADDING_LEFT, ui_px(12)},
                                      {UI_STYLE_CORNER_RADIUS, ui_px(6)},
                                      {UI_STYLE_BACKGROUND_COLOR, ui_token_ref(T_ACCENT)},
                                      {UI_STYLE_TEXT_COLOR, ui_token_ref(T_ACCENT_TEXT)},
                                      {UI_STYLE_TRANSITION(UI_STYLE_BACKGROUND_COLOR), color_spring},
                                      {UI_STYLE_TRANSITION(UI_STYLE_TEXT_COLOR), color_spring}));
    return r;
}

static ui_result text(ui_cmd_writer *w, ui_handle node, const char *s, float size, uint16_t weight) {
    ui_text_desc t = {.set = UI_TEXT_SET_ALL, .text = s, .length = (uint32_t)strlen(s),
                      .style = {.family = 0, .weight = weight, .size = size, .color = {0, 0, 0, 1}},
                      .families = families, .family_count = 1};
    return ui_cmd_set_text(w, node, &t);
}

static ui_result node(demo *d, ui_cmd_writer *w, ui_handle *out, ui_handle parent, uint32_t style) {
    ui_result r = ui_handle_alloc(d->handles, out);
    if (r == UI_OK) r = ui_cmd_create_node(w, *out, parent);
    if (r == UI_OK) r = ui_cmd_set_styles(w, *out, &style, 1);
    return r;
}

// A new card, appended to the page and then moved before `before` (null: stays last).
static ui_result add_card(demo *d, ui_cmd_writer *w, uint32_t at) {
    card c = {.id = d->next_id++};
    char title[32], label[16];
    snprintf(title, sizeof title, "Card %u", c.id);
    snprintf(label, sizeof label, "#%03u", c.id);
    ui_result r = node(d, w, &c.node, d->page, S_CARD);
    if (r == UI_OK) r = node(d, w, &c.title, c.node, S_TITLE);
    if (r == UI_OK) r = text(w, c.title, title, 16, 650);
    if (r == UI_OK) r = node(d, w, &c.body, c.node, S_BODY);
    if (r == UI_OK) r = text(w, c.body, PARAGRAPHS[c.id % (sizeof PARAGRAPHS / sizeof *PARAGRAPHS)], 13, 400);
    if (r == UI_OK) r = node(d, w, &c.footer, c.node, S_FOOTER);
    if (r == UI_OK) r = node(d, w, &c.label, c.footer, S_LABEL);
    if (r == UI_OK) r = text(w, c.label, label, 12, 400);
    if (r == UI_OK) r = node(d, w, &c.button, c.footer, S_BUTTON);
    if (r == UI_OK) r = text(w, c.button, "Open", 12, 600);
    if (r != UI_OK) return r;
    if (at < d->count) r = ui_cmd_move_node(w, c.node, d->page, d->cards[at].node);
    memmove(&d->cards[at + 1], &d->cards[at], (d->count - at) * sizeof *d->cards);
    d->cards[at] = c;
    d->count++;
    return r;
}

static ui_result remove_card(demo *d, ui_cmd_writer *w, uint32_t at) {
    card c = d->cards[at];
    ui_result r = ui_cmd_destroy_node(w, c.node);
    ui_handle all[] = {c.node, c.title, c.body, c.footer, c.label, c.button};
    for (size_t i = 0; i < sizeof all / sizeof *all; i++) ui_handle_free(d->handles, all[i]);
    memmove(&d->cards[at], &d->cards[at + 1], (d->count - at - 1) * sizeof *d->cards);
    d->count--;
    return r;
}

static ui_result move_card(demo *d, ui_cmd_writer *w, uint32_t from, uint32_t to) {
    card c = d->cards[from];
    memmove(&d->cards[from], &d->cards[from + 1], (d->count - from - 1) * sizeof *d->cards);
    d->count--;
    ui_handle before = to < d->count ? d->cards[to].node : (ui_handle){0};
    memmove(&d->cards[to + 1], &d->cards[to], (d->count - to) * sizeof *d->cards);
    d->cards[to] = c;
    d->count++;
    return ui_cmd_move_node(w, c.node, d->page, before);
}

// Insert, move, remove, move, in turn (an insert or a remove that would leave the grid too
// full or too empty moves a card instead).
static ui_result shuffle(demo *d, ui_cmd_writer *w) {
    uint32_t kind = d->step++ % 4;
    if (kind == 0 && d->count < MAX_CARDS) return add_card(d, w, next_random(d) % (d->count + 1));
    if (kind == 2 && d->count > 4) return remove_card(d, w, next_random(d) % d->count);
    uint32_t from = next_random(d) % d->count, to = next_random(d) % d->count;
    if (to == from) to = (from + 1) % d->count;
    return move_card(d, w, from, to);
}

static ui_result create(demo *d, ui_cmd_writer *w) {
    ui_result r = set_theme(d, w);
    if (r == UI_OK) r = define_styles(w);
    if (r == UI_OK) r = node(d, w, &d->page, d->surface, S_PAGE);
    for (uint32_t i = 0; i < 10 && r == UI_OK; i++) r = add_card(d, w, d->count);
    return r;
}

static ui_result on_events(void *user, const void *events, size_t size, ui_cmd_writer *w) {
    demo *d = user;
    ui_event_reader rd;
    ui_result r = ui_event_reader_init(&rd, events, size);
    if (r != UI_OK) return r;
    ui_event ev;
    while ((r = ui_event_next(&rd, &ev)) == UI_OK) {
        switch (ev.type) {
        case UI_EVENT_SURFACE_RESIZED:
            // Layout follows the surface: only the first resize has anything to do.
            if (ui_handle_is_null(d->page)) {
                d->surface = ev.surface;
                if ((r = create(d, w)) != UI_OK) return r;
            }
            break;
        case UI_EVENT_FRAME_COMMITTED:
            if (!d->armed) {
                d->armed = true;
                uint64_t now = ev.frame_committed.predicted_present_ns;
                if (d->shuffle && (r = ui_cmd_set_timer(w, SHUFFLE_TIMER, now + SHUFFLE_NS)) != UI_OK) return r;
                if (d->theme && (r = ui_cmd_set_timer(w, THEME_TIMER, now + THEME_NS)) != UI_OK) return r;
            }
            break;
        case UI_EVENT_TIMER_FIRED:
            if (ev.timer_fired.id == SHUFFLE_TIMER) {
                r = shuffle(d, w);
                if (r == UI_OK) r = ui_cmd_set_timer(w, SHUFFLE_TIMER, ev.timer_fired.deadline_ns + SHUFFLE_NS);
            } else {
                d->dark = !d->dark;
                r = set_theme(d, w);
                if (r == UI_OK) r = ui_cmd_set_timer(w, THEME_TIMER, ev.timer_fired.deadline_ns + THEME_NS);
            }
            if (r != UI_OK) return r;
            break;
        default:
            break;
        }
    }
    return r == UI_ERR_END ? UI_OK : r;
}

int main(int argc, char **argv) {
    bool headless = false;
    double at = 0;
    uint32_t scale = 150;
    const char *out = "layout_cards.ppm";
    demo d = {.seed = 7, .shuffle = true, .theme = true};
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless")) headless = true;
        else if (!strcmp(argv[i], "--no-shuffle")) d.shuffle = false;
        else if (!strcmp(argv[i], "--no-theme")) d.theme = false;
        else if (!strcmp(argv[i], "--dark")) d.dark = true;
        else if (!strcmp(argv[i], "--at") && i + 1 < argc) at = atof(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = (uint32_t)atoi(argv[++i]);
        else {
            fprintf(stderr,
                    "usage: %s [--no-shuffle] [--no-theme] [--dark]\n"
                    "       [--headless [--out f.ppm] [--scale N] [--at S]]\n",
                    argv[0]);
            return 2;
        }
    }
    if (ui_handle_allocator_create(&d.handles) != UI_OK) return 1;
    ui_app_desc desc = {
        .surface = {.kind = headless ? UI_SURFACE_HEADLESS : UI_SURFACE_WINDOW,
                    .width = 980, .height = 640, .scale_120 = headless ? scale : 0,
                    .title = "Axe — layout_cards"},
        .logic = {.user = &d, .on_events = on_events},
        .start_paused = headless,
        .text = {.font_files = font_files, .font_file_count = 1, .system_fonts = true},
    };
    ui_app *app;
    ui_result r = ui_app_create(&desc, &app);
    if (r != UI_OK) {
        fprintf(stderr, "cannot start: %s\n", ui_result_str(r));
        ui_handle_allocator_destroy(d.handles);
        return 1;
    }
    int status = 0;
    if (headless) {
        // Two frames build the grid; then 60 Hz steps of the paused clock up to --at.
        r = ui_app_frame(app);
        if (r == UI_OK) r = ui_app_frame(app);
        const uint64_t frame_ns = 16666667;
        for (uint64_t t = 0; r == UI_OK && (double)t < at * 1e9; t += frame_ns) {
            ui_app_clock_step(app, frame_ns);
            r = ui_app_frame(app);
        }
        ui_image image;
        if (r == UI_OK) r = ui_app_frame(app);
        if (r == UI_OK) r = ui_surface_capture(app, ui_app_main_surface(app), &image);
        if (r == UI_OK) r = ui_image_write_ppm(&image, out);
        if (r == UI_OK) {
            printf("wrote %s (%ux%u)\n", out, image.width, image.height);
            ui_image_free(&image);
        } else {
            fprintf(stderr, "headless frame failed: %s\n", ui_result_str(r));
            status = 1;
        }
    } else {
        r = ui_app_run(app);
        if (r != UI_OK) {
            fprintf(stderr, "run failed: %s\n", ui_result_str(r));
            status = 1;
        }
    }
    ui_app_destroy(app);
    ui_handle_allocator_destroy(d.handles);
    return status;
}
