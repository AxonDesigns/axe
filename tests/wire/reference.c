// The encoder's reference cases: command buffers the C writer encodes, and an event buffer of
// every event the C codec encodes. Each language's wire tests encode the same ops and compare
// bytes (ts/packages/wire/test/wire.test.ts, tests/luau/wire.test.luau).
#include "wire/reference.h"

#include <stdlib.h>
#include <string.h>

#include "runtime/common.h"
#include "ui/ui.h"
#include "wire/gen/wire_gen.h"

static ui_handle node(uint32_t slot, uint32_t gen) { return (ui_handle){slot, gen}; }
static const ui_handle surface = {(uint32_t)UI_HANDLE_SURFACE << UI_HANDLE_SLOT_BITS, 1};

static void encode_case(ui_cmd_writer *w, const char *name) {
    ui_handle a = node(1, 1), b = node(2, 3);
    if (!strcmp(name, "basic")) {
        ui_cmd_create_node(w, a, surface);
        ui_cmd_create_node(w, b, a);
        ui_cmd_set_rect(w, a, 1.5f, -2, 300, 40.25f);
        ui_cmd_set_corner_radius(w, a, 8);
        ui_cmd_set_color(w, a, 0.1f, 0.2f, 0.3f, 1);
        ui_cmd_set_clip(w, a, true);
        ui_cmd_move_node(w, b, surface, (ui_handle){0});
        ui_cmd_move_node(w, b, a, a);
        ui_cmd_set_state(w, b, 3, 1);
        ui_cmd_request_layout(w, b, 77);
        ui_cmd_set_timer(w, 9, 123456789012345ull);
        ui_cmd_cancel_timer(w, 9);
        ui_cmd_load_font(w, 4, "/fonts/Noto \xC3\xA9.ttf");
        ui_cmd_destroy_style(w, 5);
        ui_cmd_set_styles(w, b, (uint32_t[]){3, 1, 2}, 3);
        ui_cmd_set_styles(w, b, NULL, 0);
        ui_cmd_start_spring(w, a, UI_PROP_RECT, (float[]){1, 2, 3, 4}, 170, 18.25f);
        ui_cmd_start_spring(w, a, UI_PROP_CORNER_RADIUS, (float[]){6}, 90, 19);
        ui_cmd_destroy_node(w, b);
    } else if (!strcmp(name, "styles")) {
        ui_style_decl base[] = {
            {UI_STYLE_WIDTH, ui_px(208)},
            {UI_STYLE_HEIGHT, ui_percent(50)},
            {UI_STYLE_FLEX_GROW, ui_number(1)},
            {UI_STYLE_MARGIN_LEFT, ui_auto()},
            {UI_STYLE_MAX_WIDTH, ui_none()},
            {UI_STYLE_BACKGROUND_COLOR, ui_rgba(0.2f, 0.45f, 0.95f, 1)},
            {UI_STYLE_FLEX_DIRECTION, ui_keyword(UI_FLEX_COLUMN)},
            {UI_STYLE_TEXT_COLOR, ui_token_ref(3)},
            {UI_STYLE_FONT_FAMILY, ui_string("Noto Sans, sans-serif")},
            {UI_STYLE_LANGUAGE, ui_string("sr-Cyrl")},
            {UI_STYLE_DIRECTION, ui_inherit()},
            {UI_STYLE_OPACITY, ui_initial()},
            {UI_STYLE_TRANSITION(UI_STYLE_BACKGROUND_COLOR), ui_transition(90, 19)},
            {UI_STYLE_TRANSITION(UI_STYLE_LAYOUT), ui_transition(170, 24)},
        };
        ui_style_decl hover[] = {{UI_STYLE_BACKGROUND_COLOR, ui_token_ref(7)}};
        ui_style_block_desc blocks[] = {{.decls = base, .count = UI_ARRAY_LEN(base)}, {.states = UI_STATE_HOVER | UI_STATE_PRESSED, .decls = hover, .count = 1}};
        ui_cmd_define_style(w, 1, blocks, 2);
        ui_cmd_define_style(w, 2, NULL, 0);
        ui_style_decl overrides[] = {{UI_STYLE_BORDER_WIDTH, ui_px(2)}, {UI_STYLE_WIDTH, ui_unset()}};
        ui_cmd_set_props(w, a, overrides, 2);
        ui_cmd_set_token(w, 3, ui_rgba(1, 1, 1, 1));
        ui_cmd_set_token(w, 4, ui_token_ref(3));
        ui_cmd_set_token(w, 5, ui_string("x"));
        ui_cmd_set_token(w, 3, ui_unset());
    } else if (!strcmp(name, "scopes")) {
        // Token declarations: a block with some (12-byte header), one without (8), and a node's.
        ui_style_decl base[] = {{UI_STYLE_BACKGROUND_COLOR, ui_token_ref(3)}};
        ui_token_decl scope[] = {{3, ui_rgba(0.25f, 0.5f, 1, 1)}, {4, ui_token_ref(5)}, {6, ui_string("Noto Serif")}};
        ui_token_decl hover[] = {{3, ui_px(2)}};
        ui_style_block_desc blocks[] = {{.decls = base, .count = 1, .tokens = scope, .token_count = 3},
                                        {.states = UI_STATE_HOVER, .tokens = hover, .token_count = 1},
                                        {.states = UI_STATE_PRESSED, .decls = base, .count = 1}};
        ui_cmd_define_style(w, 1, blocks, 3);
        ui_token_decl own[] = {{3, ui_rgba(1, 0, 0, 1)}, {9, ui_unset()}, {10, ui_keyword(2)}};
        ui_cmd_set_token_overrides(w, a, own, 3);
        ui_cmd_set_token_overrides(w, a, NULL, 0);
    } else if (!strcmp(name, "text")) {
        // As @axe/core's setText writes it: one family list, a language, spans, an ellipsis.
        static const char *const families[] = {"Noto Sans, sans-serif", "Noto Serif"};
        static const char *const languages[] = {"ro"};
        const char *s = "h\xC3\xA9llo \xF0\x9F\x98\x80 world";
        ui_text_span_desc spans[] = {
            {.start = 1, .end = 3, .style = {.weight = 700}, .set = UI_TEXT_SET_WEIGHT},
            {.start = 7, .end = 11, .style = {.family = 1, .language = 1, .size = 20},
             .set = UI_TEXT_SET_FAMILY | UI_TEXT_SET_LANGUAGE | UI_TEXT_SET_SIZE},
        };
        ui_text_desc t = {
            .text = s, .length = (uint32_t)strlen(s),
            .style = {.family = 0, .size = 16, .weight = 400, .color = {0, 0, 0, 1}},
            .spans = spans, .span_count = 2, .families = families, .family_count = 2,
            .languages = languages, .language_count = 1, .max_lines = 2, .ellipsis = "...",
            .align = UI_TEXT_ALIGN_CENTER, .line_height = 1.4f,
            .set = UI_TEXT_SET_FAMILY | UI_TEXT_SET_SIZE | UI_TEXT_SET_WEIGHT | UI_TEXT_SET_ALIGN |
                   UI_TEXT_SET_LINE_HEIGHT,
        };
        ui_cmd_set_text(w, a, &t);
        ui_text_desc plain = {.text = "Open", .length = 4};
        ui_cmd_set_text(w, b, &plain);
    } else if (!strcmp(name, "templates")) {
        ui_cmd_writer *ops;
        ui_cmd_writer_create(&ops);
        ui_cmd_create_node(ops, ui_template_node(0), (ui_handle){0});
        ui_cmd_create_node(ops, ui_template_node(1), ui_template_node(0));
        ui_cmd_set_state(ops, ui_template_node(1), UI_STATE_HOVER, UI_STATE_HOVER);
        ui_cmd_define_template(w, 7, 2, ops);
        ui_cmd_writer_destroy(ops);
        ui_cmd_instantiate_template(w, 7, surface, (ui_handle){0}, (ui_handle[]){node(3, 1), node(4, 1)}, 2);
        ui_cmd_instantiate_template(w, 7, a, b, (ui_handle[]){node(5, 2), node(6, 1)}, 2);
        ui_cmd_destroy_template(w, 7);
    } else if (!strcmp(name, "long_text")) {
        // Longer than one op: set_text and append_text ops.
        static char big[200000];
        for (size_t i = 0; i < sizeof big; i++) big[i] = (char)('a' + i % 26);
        ui_cmd_set_text(w, a, &(ui_text_desc){.text = big, .length = sizeof big, .ellipsis = ""});
    }
}

bool ui_test_reference_commands(const char *name, uint8_t **bytes, size_t *size) {
    ui_cmd_writer *w;
    ui_cmd_writer_create(&w);
    encode_case(w, name);
    bool ok = ui_cmd_writer_take(w, bytes, size) == UI_OK;
    ui_cmd_writer_destroy(w);
    return ok;
}

uint8_t *ui_test_reference_events(size_t *size) {
    ui_wire_buf b;
    ui_wire_buf_init(&b, UI_WIRE_MAGIC_EVENT);
    ui_wire_clock_put(&b, &(ui_wire_clock){(1ull << 60) + 12345});
    ui_wire_frame_committed_put(&b, &(ui_wire_frame_committed){surface, 42, 16666667});
    ui_wire_surface_resized_put(&b, &(ui_wire_surface_resized){surface, 980, 640.5f, 150});
    ui_wire_surface_close_requested_put(&b, &(ui_wire_surface_close_requested){surface});
    ui_wire_timer_fired_put(&b, &(ui_wire_timer_fired){7, 2500000000ull, 2516666667ull});
    ui_wire_font_loaded_put(&b, &(ui_wire_font_loaded){3, UI_ERR_IO});
    ui_wire_layout_measured m = {.request_id = 9, .result = UI_OK, .node = node(5, 2), .surface = surface};
    ui_wire_rect *rects[4] = {&m.layout_in_parent, &m.layout_in_surface, &m.presentation_in_parent,
                              &m.presentation_in_surface};
    for (int i = 0; i < 4; i++) *rects[i] = (ui_wire_rect){(float)i, (float)i + 0.5f, 100.25f, 20};
    ui_wire_layout_measured_put(&b, &m);
    uint8_t *future = ui_wire_buf_op(&b, 999, 8); // from a newer core: skipped
    memset(future, 0xAB, 8);
    return ui_wire_buf_finish(&b, size);
}
