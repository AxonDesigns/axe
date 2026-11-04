// layout_cards: a wrapping grid of cards laid out by flexbox. Every 2.5 s a card is inserted,
// moved or removed, and the cards slide to their new places (layout animations); every 4 s
// the theme switches between light and dark, and every color springs to its new value
// (theme tokens with color transitions); every 6 s the page turns between left-to-right and
// right-to-left, and the cards slide to their mirrored places. Cards have a thicker border
// on their inline-start side and buttons rounder start corners (logical properties), so
// both follow the direction. Resizing the window reflows the grid, animated too.
//
// Token scopes (docs/styles.md): a panel at the top declares every theme token as an alias
// of the other theme's, so it and its cards show the inverted scheme and flip the other way
// when the theme switches. A card's hover state re-themes its subtree the same way; every
// 1.5 s the next card is hovered (input comes later), the panel's included, where the
// hover's aliases resolve through the panel's scope.
//
//   layout_cards [--no-shuffle] [--no-theme] [--no-direction] [--no-hover] [--rtl]   window
//   layout_cards --headless [--out f.ppm] [--scale N] [--at S] [--dark] [--rtl]
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
#define DIRECTION_TIMER 3
#define HOVER_TIMER 4
#define SHUFFLE_NS 2500000000ull
#define THEME_NS 4000000000ull
#define DIRECTION_NS 6000000000ull
#define HOVER_NS 1500000000ull
#define PANEL_CARDS 2

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

// Theme tokens and the styles that use them. The inverse tokens (T_x + INVERSE) hold the
// other theme's values, which the panel's scope aliases.
enum { T_PAGE = 1, T_CARD, T_BORDER, T_TITLE, T_BODY, T_MUTED, T_ACCENT, T_ACCENT_TEXT, T_HOVER, T_COUNT };
enum { INVERSE = T_COUNT - 1 };
enum { S_PAGE = 1, S_CARD, S_TITLE, S_BODY, S_FOOTER, S_LABEL, S_BUTTON, S_PANEL };

static const float THEMES[2][T_COUNT][4] = {
    {{0}, {0.93f, 0.94f, 0.96f, 1}, {1, 1, 1, 1}, {0.84f, 0.86f, 0.9f, 1}, {0.1f, 0.11f, 0.14f, 1},
     {0.3f, 0.32f, 0.36f, 1}, {0.5f, 0.52f, 0.56f, 1}, {0.2f, 0.45f, 0.95f, 1}, {1, 1, 1, 1},
     {0.87f, 0.92f, 1, 1}},
    {{0}, {0.07f, 0.08f, 0.1f, 1}, {0.13f, 0.14f, 0.17f, 1}, {0.24f, 0.26f, 0.31f, 1},
     {0.93f, 0.94f, 0.96f, 1}, {0.72f, 0.74f, 0.78f, 1}, {0.55f, 0.57f, 0.62f, 1},
     {0.38f, 0.62f, 1, 1}, {0.05f, 0.06f, 0.08f, 1}, {0.12f, 0.18f, 0.31f, 1}},
};

typedef struct card {
    ui_handle node, title, body, footer, label, button;
    uint32_t id;
} card;

typedef struct demo {
    ui_handle_allocator *handles;
    ui_handle surface, page, panel, heading;
    card cards[MAX_CARDS]; // in display order
    card panel_cards[PANEL_CARDS];
    uint32_t count, next_id, step, hover_step;
    ui_handle hovered;     // the card hovered now (null: none)
    uint32_t seed;
    bool dark, rtl, shuffle, theme, direction, hover, armed;
} demo;

static uint32_t next_random(demo *d) {
    d->seed = d->seed * 1664525u + 1013904223u;
    return d->seed >> 8;
}

// Every theme token, then every inverse one (the other theme's values).
static ui_result set_theme(demo *d, ui_cmd_writer *w) {
    ui_result r = UI_OK;
    for (int inverse = 0; inverse < 2; inverse++) {
        for (int t = T_PAGE; t < T_COUNT && r == UI_OK; t++) {
            const float *c = THEMES[d->dark != inverse][t];
            r = ui_cmd_set_token(w, (uint32_t)(t + inverse * INVERSE), ui_rgba(c[0], c[1], c[2], c[3]));
        }
    }
    return r;
}

// The page's direction, which every card, paragraph and logical property follows.
static ui_result set_direction(demo *d, ui_cmd_writer *w) {
    ui_style_decl dir = {UI_STYLE_DIRECTION, ui_keyword(d->rtl ? UI_DIRECTION_RTL : UI_DIRECTION_LTR)};
    return ui_cmd_set_props(w, d->page, &dir, 1);
}

#define DECLS(...) ((ui_style_decl[]){__VA_ARGS__}), (sizeof((ui_style_decl[]){__VA_ARGS__}) / sizeof(ui_style_decl))

static ui_result define(ui_cmd_writer *w, uint32_t id, const ui_style_decl *decls, uint32_t count) {
    return ui_cmd_define_style(w, id, &(ui_style_block_desc){.decls = decls, .count = count}, 1);
}

// The panel's scope: every theme token is the other theme's. A hovered card's: the card
// takes the hover color, its border and title the accent (resolved where they are used, so
// in the panel they are the panel's).
static const ui_token_decl PANEL_TOKENS[] = {
    {T_PAGE, {.kind = UI_VALUE_TOKEN, .token = T_PAGE + INVERSE}},
    {T_CARD, {.kind = UI_VALUE_TOKEN, .token = T_CARD + INVERSE}},
    {T_BORDER, {.kind = UI_VALUE_TOKEN, .token = T_BORDER + INVERSE}},
    {T_TITLE, {.kind = UI_VALUE_TOKEN, .token = T_TITLE + INVERSE}},
    {T_BODY, {.kind = UI_VALUE_TOKEN, .token = T_BODY + INVERSE}},
    {T_MUTED, {.kind = UI_VALUE_TOKEN, .token = T_MUTED + INVERSE}},
    {T_ACCENT, {.kind = UI_VALUE_TOKEN, .token = T_ACCENT + INVERSE}},
    {T_ACCENT_TEXT, {.kind = UI_VALUE_TOKEN, .token = T_ACCENT_TEXT + INVERSE}},
    {T_HOVER, {.kind = UI_VALUE_TOKEN, .token = T_HOVER + INVERSE}},
};
static const ui_token_decl HOVER_TOKENS[] = {
    {T_CARD, {.kind = UI_VALUE_TOKEN, .token = T_HOVER}},
    {T_BORDER, {.kind = UI_VALUE_TOKEN, .token = T_ACCENT}},
    {T_TITLE, {.kind = UI_VALUE_TOKEN, .token = T_ACCENT}},
};

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
    ui_style_decl card[] = {{UI_STYLE_WIDTH, ui_px(208)},
                            {UI_STYLE_FLEX_DIRECTION, ui_keyword(UI_FLEX_COLUMN)},
                            {UI_STYLE_PADDING_TOP, ui_px(14)}, {UI_STYLE_PADDING_RIGHT, ui_px(14)},
                            {UI_STYLE_PADDING_BOTTOM, ui_px(12)}, {UI_STYLE_PADDING_LEFT, ui_px(14)},
                            {UI_STYLE_ROW_GAP, ui_px(8)}, {UI_STYLE_BORDER_WIDTH, ui_px(1)},
                            {UI_STYLE_CORNER_RADIUS, ui_px(10)},
                            {UI_STYLE_BACKGROUND_COLOR, ui_token_ref(T_CARD)},
                            {UI_STYLE_BORDER_COLOR, ui_token_ref(T_BORDER)},
                            {UI_STYLE_BORDER_START_WIDTH, ui_px(4)},
                            {UI_STYLE_BORDER_START_COLOR, ui_token_ref(T_ACCENT)},
                            {UI_STYLE_TRANSITION(UI_STYLE_BACKGROUND_COLOR), color_spring},
                            {UI_STYLE_TRANSITION(UI_STYLE_BORDER_COLOR), color_spring},
                            {UI_STYLE_TRANSITION(UI_STYLE_LAYOUT), ui_transition(170, 24)}};
    ui_style_block_desc card_blocks[] = {
        {.decls = card, .count = sizeof card / sizeof *card},
        {.states = UI_STATE_HOVER, .tokens = HOVER_TOKENS, .token_count = sizeof HOVER_TOKENS / sizeof *HOVER_TOKENS},
    };
    if (r == UI_OK) r = ui_cmd_define_style(w, S_CARD, card_blocks, 2);
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
                                      {UI_STYLE_CORNER_RADIUS, ui_px(3)},
                                      {UI_STYLE_CORNER_RADIUS_TOP_START, ui_px(11)},
                                      {UI_STYLE_CORNER_RADIUS_BOTTOM_START, ui_px(11)},
                                      {UI_STYLE_BACKGROUND_COLOR, ui_token_ref(T_ACCENT)},
                                      {UI_STYLE_TEXT_COLOR, ui_token_ref(T_ACCENT_TEXT)},
                                      {UI_STYLE_TRANSITION(UI_STYLE_BACKGROUND_COLOR), color_spring},
                                      {UI_STYLE_TRANSITION(UI_STYLE_TEXT_COLOR), color_spring}));
    // The panel: a full row of the page, in the inverted scheme (its scope), whose own
    // colors read the same tokens as the page's and cards'.
    ui_style_decl panel[] = {{UI_STYLE_WIDTH, ui_percent(100)},
                             {UI_STYLE_FLEX_WRAP, ui_keyword(UI_FLEX_WRAP)},
                             {UI_STYLE_ALIGN_ITEMS, ui_keyword(UI_FLEX_START)},
                             {UI_STYLE_PADDING_TOP, ui_px(16)}, {UI_STYLE_PADDING_RIGHT, ui_px(16)},
                             {UI_STYLE_PADDING_BOTTOM, ui_px(16)}, {UI_STYLE_PADDING_LEFT, ui_px(16)},
                             {UI_STYLE_ROW_GAP, ui_px(12)}, {UI_STYLE_COLUMN_GAP, ui_px(14)},
                             {UI_STYLE_CORNER_RADIUS, ui_px(14)}, {UI_STYLE_BORDER_WIDTH, ui_px(1)},
                             {UI_STYLE_BACKGROUND_COLOR, ui_token_ref(T_PAGE)},
                             {UI_STYLE_BORDER_COLOR, ui_token_ref(T_BORDER)},
                             {UI_STYLE_TRANSITION(UI_STYLE_BACKGROUND_COLOR), color_spring},
                             {UI_STYLE_TRANSITION(UI_STYLE_BORDER_COLOR), color_spring}};
    ui_style_block_desc panel_block = {.decls = panel, .count = sizeof panel / sizeof *panel, .tokens = PANEL_TOKENS,
                                       .token_count = sizeof PANEL_TOKENS / sizeof *PANEL_TOKENS};
    if (r == UI_OK) r = ui_cmd_define_style(w, S_PANEL, &panel_block, 1);
    return r;
}

static ui_result text(ui_cmd_writer *w, ui_handle node, const char *s, float size, uint16_t weight) {
    // Everything but the color (from the styles) and the direction (from the page).
    ui_text_desc t = {.set = UI_TEXT_SET_ALL & ~(UI_TEXT_SET_COLOR | UI_TEXT_SET_DIRECTION), .text = s, .length = (uint32_t)strlen(s),
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

// A card's nodes, appended to `parent`.
static ui_result make_card(demo *d, ui_cmd_writer *w, card *out, ui_handle parent) {
    card c = {.id = d->next_id++};
    char title[32], label[16];
    snprintf(title, sizeof title, "Card %u", c.id);
    snprintf(label, sizeof label, "#%03u", c.id);
    ui_result r = node(d, w, &c.node, parent, S_CARD);
    if (r == UI_OK) r = node(d, w, &c.title, c.node, S_TITLE);
    if (r == UI_OK) r = text(w, c.title, title, 16, 650);
    if (r == UI_OK) r = node(d, w, &c.body, c.node, S_BODY);
    if (r == UI_OK) r = text(w, c.body, PARAGRAPHS[c.id % (sizeof PARAGRAPHS / sizeof *PARAGRAPHS)], 13, 400);
    if (r == UI_OK) r = node(d, w, &c.footer, c.node, S_FOOTER);
    if (r == UI_OK) r = node(d, w, &c.label, c.footer, S_LABEL);
    if (r == UI_OK) r = text(w, c.label, label, 12, 400);
    if (r == UI_OK) r = node(d, w, &c.button, c.footer, S_BUTTON);
    if (r == UI_OK) r = text(w, c.button, "Open", 12, 600);
    *out = c;
    return r;
}

// A new card, appended to the page and then moved before `before` (null: stays last).
static ui_result add_card(demo *d, ui_cmd_writer *w, uint32_t at) {
    card c;
    ui_result r = make_card(d, w, &c, d->page);
    if (r != UI_OK) return r;
    if (at < d->count) r = ui_cmd_move_node(w, c.node, d->page, d->cards[at].node);
    memmove(&d->cards[at + 1], &d->cards[at], (d->count - at) * sizeof *d->cards);
    d->cards[at] = c;
    d->count++;
    return r;
}

static ui_result remove_card(demo *d, ui_cmd_writer *w, uint32_t at) {
    card c = d->cards[at];
    if (ui_handle_eq(c.node, d->hovered)) d->hovered = (ui_handle){0};
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

// Hovers the next card in turn (the panel's first), after un-hovering the last.
static ui_result hover_next(demo *d, ui_cmd_writer *w) {
    ui_result r = UI_OK;
    if (!ui_handle_is_null(d->hovered)) r = ui_cmd_set_state(w, d->hovered, UI_STATE_HOVER, 0);
    uint32_t i = d->hover_step++ % (PANEL_CARDS + d->count);
    d->hovered = i < PANEL_CARDS ? d->panel_cards[i].node : d->cards[i - PANEL_CARDS].node;
    if (r == UI_OK) r = ui_cmd_set_state(w, d->hovered, UI_STATE_HOVER, UI_STATE_HOVER);
    return r;
}

static ui_result create(demo *d, ui_cmd_writer *w) {
    ui_result r = set_theme(d, w);
    if (r == UI_OK) r = define_styles(w);
    if (r == UI_OK) r = node(d, w, &d->page, d->surface, S_PAGE);
    if (r == UI_OK) r = set_direction(d, w);
    // The panel first: a heading and its own cards, in the inverted scheme.
    if (r == UI_OK) r = node(d, w, &d->panel, d->page, S_PANEL);
    if (r == UI_OK) r = node(d, w, &d->heading, d->panel, S_TITLE);
    if (r == UI_OK)
        r = ui_cmd_set_props(w, d->heading, &(ui_style_decl){UI_STYLE_WIDTH, ui_percent(100)}, 1);
    if (r == UI_OK) r = text(w, d->heading, "A panel in the other theme: its scope aliases every token", 15, 650);
    for (uint32_t i = 0; i < PANEL_CARDS && r == UI_OK; i++) r = make_card(d, w, &d->panel_cards[i], d->panel);
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
                if (d->direction && (r = ui_cmd_set_timer(w, DIRECTION_TIMER, now + DIRECTION_NS)) != UI_OK)
                    return r;
                if (d->hover && (r = ui_cmd_set_timer(w, HOVER_TIMER, now + HOVER_NS)) != UI_OK) return r;
            }
            break;
        case UI_EVENT_TIMER_FIRED:
            if (ev.timer_fired.id == SHUFFLE_TIMER) {
                r = shuffle(d, w);
                if (r == UI_OK) r = ui_cmd_set_timer(w, SHUFFLE_TIMER, ev.timer_fired.deadline_ns + SHUFFLE_NS);
            } else if (ev.timer_fired.id == THEME_TIMER) {
                d->dark = !d->dark;
                r = set_theme(d, w);
                if (r == UI_OK) r = ui_cmd_set_timer(w, THEME_TIMER, ev.timer_fired.deadline_ns + THEME_NS);
            } else if (ev.timer_fired.id == HOVER_TIMER) {
                r = hover_next(d, w);
                if (r == UI_OK) r = ui_cmd_set_timer(w, HOVER_TIMER, ev.timer_fired.deadline_ns + HOVER_NS);
            } else {
                d->rtl = !d->rtl;
                r = set_direction(d, w);
                if (r == UI_OK) r = ui_cmd_set_timer(w, DIRECTION_TIMER, ev.timer_fired.deadline_ns + DIRECTION_NS);
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
    demo d = {.seed = 7, .shuffle = true, .theme = true, .direction = true, .hover = true};
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless")) headless = true;
        else if (!strcmp(argv[i], "--no-shuffle")) d.shuffle = false;
        else if (!strcmp(argv[i], "--no-theme")) d.theme = false;
        else if (!strcmp(argv[i], "--no-direction")) d.direction = false;
        else if (!strcmp(argv[i], "--no-hover")) d.hover = false;
        else if (!strcmp(argv[i], "--dark")) d.dark = true;
        else if (!strcmp(argv[i], "--rtl")) d.rtl = true;
        else if (!strcmp(argv[i], "--at") && i + 1 < argc) at = atof(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = (uint32_t)atoi(argv[++i]);
        else {
            fprintf(stderr,
                    "usage: %s [--no-shuffle] [--no-theme] [--no-direction] [--no-hover] [--dark] [--rtl]\n"
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
