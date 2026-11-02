// set_text / append_text on the wire: encoding, chunking of long text into continuation
// ops, U+FFFD replacement (counted, spans kept on their characters), framing errors that
// reject the buffer, and semantic errors that drop the op.
#include <stdlib.h>
#include <string.h>

#include "core/scene.h"
#include "test.h"
#include "ui/ui.h"
#include "wire/wire.h"

static ui_logger quiet = {UI_LOG_ERROR};
static const ui_handle surface = {(uint32_t)UI_HANDLE_SURFACE << UI_HANDLE_SLOT_BITS, 1};
static const ui_handle node = {1, 1};

static void setup(ui_scene *s) {
    ui_scene_init(s, &quiet);
    CHECK(ui_scene_add_root(s, surface) == UI_OK);
    ui_cmd_writer *w;
    ui_cmd_writer_create(&w);
    ui_cmd_create_node(w, node, surface);
    uint8_t *b;
    size_t n;
    ui_cmd_writer_take(w, &b, &n);
    CHECK(ui_scene_apply(s, b, n, 0) == UI_OK);
    free(b);
    ui_cmd_writer_destroy(w);
}

static ui_paragraph *text_of(ui_scene *s) { return s->nodes[ui_handle_slot(node)].text; }

static ui_text_desc basic(const char *text, uint32_t len) {
    static const char *const families[] = {"Noto Sans, sans-serif", "Mono"};
    return (ui_text_desc){
        .text = text, .length = len,
        .style = {.family = 0, .weight = 400, .size = 16, .color = {1, 1, 1, 1}},
        .families = families, .family_count = 2,
    };
}

static ui_result apply(ui_scene *s, ui_cmd_writer *w) {
    uint8_t *b;
    size_t n;
    if (ui_cmd_writer_take(w, &b, &n) != UI_OK) return UI_ERR_END;
    ui_result r = ui_scene_apply(s, b, n, 0);
    free(b);
    return r;
}

static void test_basic(void) {
    ui_scene s;
    setup(&s);
    ui_cmd_writer *w;
    ui_cmd_writer_create(&w);
    ui_text_span_desc spans[2] = {
        {0, 5, {.family = 1, .weight = 700, .slant = UI_TEXT_ITALIC, .size = 20, .color = {1, 0, 0, 1}}},
        {6, 11, {.family = UI_TEXT_FAMILY_DEFAULT, .weight = 300, .size = 12, .color = {0, 0, 1, 0.5f}}},
    };
    ui_text_desc d = basic("Hello world", 11);
    d.spans = spans;
    d.span_count = 2;
    d.direction = UI_TEXT_DIR_RTL;
    d.max_lines = 3;
    CHECK(ui_cmd_set_text(w, node, &d) == UI_OK);
    CHECK_EQ_INT(ui_cmd_writer_op_count(w), 1);
    CHECK(apply(&s, w) == UI_OK);
    ui_paragraph *p = text_of(&s);
    CHECK(p != NULL);
    if (p) {
        CHECK(p->len == 11 && !memcmp(p->text, "Hello world", 11));
        CHECK_EQ_INT(p->span_count, 2);
        CHECK_EQ_INT(p->spans[0].style.weight, 700);
        CHECK_EQ_INT(p->spans[0].style.style, UI_FONT_ITALIC);
        CHECK_EQ_INT(p->spans[1].start, 6);
        CHECK_NEAR(p->spans[1].style.color[2], 1.0, 1e-6);  // sRGB 1.0 -> linear 1.0
        CHECK_NEAR(p->spans[1].style.color[3], 0.5, 1e-6);  // alpha stays linear
        CHECK_EQ_INT(p->family_count, 2);
        CHECK(!memcmp(p->families + p->family_offsets[0], "Noto Sans, sans-serif", 21));
        CHECK(!memcmp(p->families + p->family_offsets[1], "Mono", 4));
        CHECK_EQ_INT(p->dir, UI_BIDI_RTL);
        CHECK_EQ_INT(p->max_lines, 3);
        CHECK(p->ellipsis_len == 3 && !memcmp(p->ellipsis, "\xE2\x80\xA6", 3)); // default
    }
    // A custom ellipsis, including "none".
    d = basic("x", 1);
    d.ellipsis = "...";
    CHECK(ui_cmd_set_text(w, node, &d) == UI_OK && apply(&s, w) == UI_OK);
    CHECK(text_of(&s)->ellipsis_len == 3 && !memcmp(text_of(&s)->ellipsis, "...", 3));
    d.ellipsis = "";
    CHECK(ui_cmd_set_text(w, node, &d) == UI_OK && apply(&s, w) == UI_OK);
    CHECK_EQ_INT(text_of(&s)->ellipsis_len, 0);
    CHECK_EQ_INT(s.stats.dropped, 0);
    ui_cmd_writer_destroy(w);
    ui_scene_destroy(&s);
}

static void test_long_text(void) {
    ui_scene s;
    setup(&s);
    ui_cmd_writer *w;
    ui_cmd_writer_create(&w);
    uint32_t len = 200000;
    char *big = malloc(len);
    for (uint32_t i = 0; i < len; i++) big[i] = (char)('a' + i % 26);
    ui_text_desc d = basic(big, len);
    CHECK(ui_cmd_set_text(w, node, &d) == UI_OK);
    CHECK_EQ_INT(ui_cmd_writer_op_count(w), 4); // set_text + 3 append_text
    CHECK(apply(&s, w) == UI_OK);
    CHECK(text_of(&s) && text_of(&s)->len == len && !memcmp(text_of(&s)->text, big, len));
    CHECK_EQ_INT(s.stats.dropped, 0);
    CHECK_EQ_INT(s.stats.applied, 2); // create_node + one set_text (continuations included)

    // Without its continuations, a set_text is dropped and the node keeps its old text.
    CHECK(ui_cmd_set_text(w, node, &d) == UI_OK);
    uint8_t *b;
    size_t n;
    ui_cmd_writer_take(w, &b, &n);
    ui_wire_reader r;
    ui_wire_reader_init(&r, UI_WIRE_MAGIC_CMD, b, n);
    ui_wire_op op;
    ui_wire_buf cut;
    ui_wire_buf_init(&cut, UI_WIRE_MAGIC_CMD);
    for (int i = 0; ui_wire_reader_next(&r, &op) == UI_OK; i++) {
        if (i == 2) continue; // lose the second chunk
        uint8_t *p = ui_wire_buf_op(&cut, op.opcode, op.payload_size);
        memcpy(p, op.payload, op.payload_size);
    }
    free(b);
    b = ui_wire_buf_finish(&cut, &n);
    uint64_t dropped = s.stats.dropped;
    CHECK(ui_scene_apply(&s, b, n, 0) == UI_OK);
    CHECK(s.stats.dropped > dropped);
    CHECK(text_of(&s)->len == len); // unchanged
    free(b);

    // A continuation with nothing before it is dropped.
    ui_wire_buf stray;
    ui_wire_buf_init(&stray, UI_WIRE_MAGIC_CMD);
    uint8_t *p = ui_wire_buf_op(&stray, UI_OP_APPEND_TEXT, 16);
    p = ui_wire_put_handle(p, node);
    p = ui_wire_put_u32(p, 4);
    memcpy(p, "abcd", 4);
    b = ui_wire_buf_finish(&stray, &n);
    dropped = s.stats.dropped;
    CHECK(ui_scene_apply(&s, b, n, 0) == UI_OK);
    CHECK_EQ_INT(s.stats.dropped, dropped + 1);
    free(b);
    free(big);
    ui_cmd_writer_destroy(w);
    ui_scene_destroy(&s);
}

static void test_invalid_utf8(void) {
    ui_scene s;
    setup(&s);
    ui_cmd_writer *w;
    ui_cmd_writer_create(&w);
    // "a <FF> b <E2 82> c": two ill-formed subparts; the span covers "b".
    const char text[] = "a\xFF" "b\xE2\x82" "c";
    ui_text_span_desc span = {2, 3, {.family = 0, .weight = 700, .size = 16, .color = {1, 1, 1, 1}}};
    ui_text_desc d = basic(text, sizeof text - 1);
    d.spans = &span;
    d.span_count = 1;
    CHECK(ui_cmd_set_text(w, node, &d) == UI_OK && apply(&s, w) == UI_OK);
    ui_paragraph *p = text_of(&s);
    const char want[] = "a\xEF\xBF\xBD" "b\xEF\xBF\xBD" "c";
    CHECK(p && p->len == sizeof want - 1 && !memcmp(p->text, want, p->len));
    CHECK_EQ_INT(s.stats.text_invalid, 2);
    CHECK(p && p->span_count == 1 && p->spans[0].start == 4 && p->spans[0].end == 5);
    ui_cmd_writer_destroy(w);
    ui_scene_destroy(&s);
}

static void test_validation(void) {
    ui_scene s;
    setup(&s);
    ui_cmd_writer *w;
    ui_cmd_writer_create(&w);
    // The writer refuses bad input and stays unchanged.
    ui_text_span_desc spans[2] = {
        {0, 3, {.family = 0, .weight = 400, .size = 16, .color = {1, 1, 1, 1}}},
        {2, 4, {.family = 0, .weight = 400, .size = 16, .color = {1, 1, 1, 1}}},
    };
    ui_text_desc d = basic("abcdef", 6);
    d.spans = spans;
    d.span_count = 2;
    CHECK(ui_cmd_set_text(w, node, &d) == UI_ERR_INVALID_ARGUMENT); // overlap
    d.span_count = 0;
    d.style.weight = 0;
    CHECK(ui_cmd_set_text(w, node, &d) == UI_ERR_INVALID_ARGUMENT);
    d.style.weight = 400;
    d.style.family = 5;
    CHECK(ui_cmd_set_text(w, node, &d) == UI_ERR_INVALID_ARGUMENT);
    CHECK_EQ_INT(ui_cmd_writer_op_count(w), 0);

    // The core re-checks what a writer could get wrong: patch a valid op into an overlap.
    d.style.family = 0;
    spans[1].start = 3;
    d.spans = spans;
    d.span_count = 2;
    CHECK(ui_cmd_set_text(w, node, &d) == UI_OK);
    uint8_t *b;
    size_t n;
    ui_cmd_writer_take(w, &b, &n);
    uint8_t *second_start = b + UI_WIRE_HEADER_SIZE + UI_WIRE_OP_HEADER_SIZE +
                            UI_WIRE_SIZE_SET_TEXT_HEADER + UI_WIRE_SIZE_TEXT_SPAN;
    ui_wire_put_u32(second_start, 1);
    uint64_t dropped = s.stats.dropped;
    CHECK(ui_scene_apply(&s, b, n, 0) == UI_OK);
    CHECK_EQ_INT(s.stats.dropped, dropped + 1);
    CHECK(text_of(&s) == NULL);
    // A family region whose size is not a multiple of 4 is a framing error: the whole buffer
    // is rejected.
    ui_wire_put_u32(second_start, 3);
    uint8_t *fam_bytes = b + UI_WIRE_HEADER_SIZE + UI_WIRE_OP_HEADER_SIZE + 20;
    ui_wire_put_u16(fam_bytes, (uint16_t)(ui_wire_get_u16(fam_bytes) + 1));
    CHECK(ui_scene_apply(&s, b, n, 0) == UI_ERR_MALFORMED);
    free(b);

    // A stale handle drops the op.
    d.span_count = 0;
    CHECK(ui_cmd_set_text(w, (ui_handle){1, 7}, &d) == UI_OK);
    dropped = s.stats.dropped;
    CHECK(apply(&s, w) == UI_OK);
    CHECK_EQ_INT(s.stats.dropped, dropped + 1);
    // Destroying the node frees its text (ASan checks this).
    CHECK(ui_cmd_set_text(w, node, &d) == UI_OK && apply(&s, w) == UI_OK);
    CHECK(text_of(&s) != NULL);
    ui_cmd_destroy_node(w, node);
    CHECK(apply(&s, w) == UI_OK);
    ui_cmd_writer_destroy(w);
    ui_scene_destroy(&s);
}

int main(void) {
    RUN(test_basic);
    RUN(test_long_text);
    RUN(test_invalid_utf8);
    RUN(test_validation);
    return test_failures != 0;
}
