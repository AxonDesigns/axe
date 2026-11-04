// Wire evolution (docs/command-buffer.md, "Evolving the format"). A buffer from an older
// writer (payloads at their original sizes, without the fields appended since) and one from
// a newer writer (fields this reader doesn't know after every op's known ones: a longer
// set_text header, longer spans, trailing bytes) must apply exactly like this writer's own
// buffer. Payloads below an op's original size still reject the buffer, and event readers
// skip unknown trailing fields the same way.
#include <stdlib.h>
#include <string.h>

#include "core/scene.h"
#include "test.h"
#include "ui/ui.h"
#include "wire/gen/wire_gen.h"

static ui_logger quiet = {UI_LOG_ERROR};
static const ui_handle surface = {(uint32_t)UI_HANDLE_SURFACE << UI_HANDLE_SLOT_BITS, 1};
static const ui_handle node = {1, 1};
static const ui_handle child = {2, 1};

#define FILL 0xA5 // what a newer writer puts in fields this reader doesn't know
#define TEXT_LEN 150000u // long enough for two append_text continuations

typedef struct captured {
    uint32_t timer_id, font_id, font_len;
    uint64_t deadline;
    char font_path[64];
} captured;

static void on_timer(void *user, uint32_t id, uint64_t deadline_ns) {
    captured *c = user;
    c->timer_id = id;
    c->deadline = deadline_ns;
}

static void on_font(void *user, uint32_t id, const char *path, uint32_t len, uint32_t offset) {
    (void)offset;
    captured *c = user;
    c->font_id = id;
    c->font_len = len < sizeof c->font_path ? len : 0;
    memcpy(c->font_path, path, c->font_len);
}

// Every op this writer knows, with set_text using every field. `paragraph_fields` false
// leaves line height and alignment at their defaults, which is all an older writer, from
// before those fields, can express.
static uint8_t *write_all(bool paragraph_fields, const char *text, size_t *size) {
    ui_cmd_writer *w;
    ui_cmd_writer_create(&w);
    ui_cmd_create_node(w, node, surface);
    ui_cmd_create_node(w, child, node);
    ui_cmd_set_rect(w, node, 1, 2, 300, 40);
    ui_cmd_set_corner_radius(w, node, 6);
    ui_cmd_set_color(w, node, 0.25f, 0.5f, 0.75f, 1);
    ui_cmd_set_clip(w, node, true);
    const float target[4] = {10, 20, 30, 40};
    ui_cmd_start_spring(w, child, UI_PROP_RECT, target, 170, 18);
    ui_cmd_set_timer(w, 9, 123456789);
    ui_cmd_load_font(w, 4, "/fonts/Extra.ttf");
    static const char *const families[] = {"Noto Sans, sans-serif", "Mono"};
    static const char *const languages[] = {"ro"};
    ui_text_span_desc spans[2] = {
        {0, 5, {.family = 1, .weight = 700, .slant = UI_TEXT_ITALIC, .size = 20,
                .color = {1, 0, 0, 1}, .language = 1}, UI_TEXT_SET_SPAN_ALL},
        {6, 11, {.family = UI_TEXT_FAMILY_DEFAULT, .weight = 300, .size = 12,
                 .color = {0, 0, 1, 0.5f}}, UI_TEXT_SET_SPAN_ALL},
    };
    ui_text_desc d = {.set = UI_TEXT_SET_ALL,
        .text = text, .length = TEXT_LEN,
        .style = {.family = 0, .weight = 400, .size = 16, .color = {1, 1, 1, 1}},
        .spans = spans, .span_count = 2,
        .families = families, .family_count = 2, .languages = languages, .language_count = 1,
        .direction = UI_TEXT_DIR_RTL, .max_lines = 3, .ellipsis = "...",
        .align = paragraph_fields ? UI_TEXT_ALIGN_JUSTIFY : UI_TEXT_ALIGN_START,
        .line_height = paragraph_fields ? 1.4f : 0,
    };
    CHECK(ui_cmd_set_text(w, child, &d) == UI_OK);
    uint8_t *bytes;
    ui_cmd_writer_take(w, &bytes, size);
    ui_cmd_writer_destroy(w);
    return bytes;
}

// Re-encodes a buffer op by op, as another writer version would have.
typedef void (*rewrite_fn)(const ui_wire_op *op, ui_wire_buf *out);

static uint8_t *rewrite(const uint8_t *bytes, size_t size, rewrite_fn fn, size_t *out_size) {
    ui_wire_reader r;
    CHECK(ui_wire_reader_init(&r, UI_WIRE_MAGIC_CMD, bytes, size) == UI_OK);
    ui_wire_buf out;
    ui_wire_buf_init(&out, UI_WIRE_MAGIC_CMD);
    ui_wire_op op;
    while (ui_wire_reader_next(&r, &op) == UI_OK) fn(&op, &out);
    return ui_wire_buf_finish(&out, out_size);
}

static uint8_t *emit(ui_wire_buf *out, uint16_t opcode, uint32_t payload) {
    uint8_t *p = ui_wire_buf_op(out, opcode, payload);
    CHECK(p != NULL);
    return p;
}

// The writer before header sizes existed: set_text with the original 64-byte header and
// u32 chunk_bytes (whose high half held 0 where header_bytes now is), the default style's
// start and end 0 (where span_bytes now is), and append_text and load_font with their u32
// lengths. Every other op was already at its original size.
static void as_older(const ui_wire_op *op, ui_wire_buf *out) {
    uint8_t *p;
    switch (op->opcode) {
    case UI_OP_SET_TEXT:
        CHECK(ui_wire_get_f32(op->payload + 64) == 0 && op->payload[68] == 0); // defaults
        p = emit(out, op->opcode, op->payload_size - 8);
        memcpy(p, op->payload, 64);
        memcpy(p + 64, op->payload + 72, op->payload_size - 72u);
        ui_wire_put_u16(p + 14, 0);
        ui_wire_put_u32(p + 28, 0);
        return;
    case UI_OP_APPEND_TEXT:
    case UI_OP_LOAD_FONT:
        p = emit(out, op->opcode, op->payload_size);
        memcpy(p, op->payload, op->payload_size);
        ui_wire_put_u16(p + (op->opcode == UI_OP_LOAD_FONT ? 6 : 10), 0);
        return;
    default:
        p = emit(out, op->opcode, op->payload_size);
        memcpy(p, op->payload, op->payload_size);
    }
}

// A newer writer: 8 more header bytes and 4 more bytes per span in set_text, 4 more header
// bytes in append_text and load_font, and 8 trailing bytes on every op (after the variable
// data of the ops that have some). The extra bytes are nonzero, so reading them would show.
// Its larger ops hold less text, so the text is gathered and split into chunks again.
static struct {
    uint8_t head[UI_WIRE_MAX_PAYLOAD]; // set_text up to its first chunk
    uint32_t head_len;
    uint8_t *text;
    uint32_t have, total;
} pending;

static void put_newer_text(ui_wire_buf *out) {
    const uint8_t *in = pending.head;
    uint32_t spans = ui_wire_get_u16(in + 16), header = 72 + 8, stride = 36 + 4;
    uint32_t rest = pending.head_len - 72u - spans * 36u; // strings and ellipsis
    uint32_t fixed = header + spans * stride + rest;
    uint32_t chunk = (UI_WIRE_MAX_PAYLOAD - fixed - 8) & ~3u;
    if (chunk > pending.total) chunk = pending.total;
    uint32_t size = fixed + ui_wire_pad4(chunk) + 8;
    uint8_t *p = emit(out, UI_OP_SET_TEXT, size);
    memset(p, FILL, size);
    memcpy(p, in, 72);
    ui_wire_put_u16(p + 12, (uint16_t)chunk);
    ui_wire_put_u16(p + 14, (uint16_t)header);
    ui_wire_put_u16(p + 28, (uint16_t)stride);
    for (uint32_t i = 0; i < spans; i++) memcpy(p + header + i * stride, in + 72 + i * 36, 36);
    memcpy(p + header + spans * stride, in + 72 + spans * 36, rest);
    memcpy(p + fixed, pending.text, chunk);
    memset(p + fixed + chunk, 0, ui_wire_pad4(chunk) - chunk);
    for (uint32_t sent = chunk; sent < pending.total; sent += chunk) {
        chunk = (UI_WIRE_MAX_PAYLOAD - 16 - 8) & ~3u;
        if (chunk > pending.total - sent) chunk = pending.total - sent;
        size = 16 + ui_wire_pad4(chunk) + 8;
        p = emit(out, UI_OP_APPEND_TEXT, size);
        memset(p, FILL, size);
        memcpy(p, in, 8); // the node
        ui_wire_put_u16(p + 8, (uint16_t)chunk);
        ui_wire_put_u16(p + 10, 16);
        memcpy(p + 16, pending.text + sent, chunk);
        memset(p + 16 + chunk, 0, ui_wire_pad4(chunk) - chunk);
    }
    free(pending.text);
    pending.text = NULL;
}

static void as_newer(const ui_wire_op *op, ui_wire_buf *out) {
    const uint8_t *in = op->payload;
    uint8_t *p;
    switch (op->opcode) {
    case UI_OP_SET_TEXT: {
        uint32_t chunk = ui_wire_get_u16(in + 12);
        pending.head_len = op->payload_size - ui_wire_pad4(chunk);
        memcpy(pending.head, in, pending.head_len);
        pending.total = ui_wire_get_u32(in + 8);
        pending.text = malloc(pending.total);
        memcpy(pending.text, in + pending.head_len, chunk);
        pending.have = chunk;
        if (pending.have == pending.total) put_newer_text(out);
        return;
    }
    case UI_OP_APPEND_TEXT: {
        uint32_t chunk = ui_wire_get_u16(in + 8);
        memcpy(pending.text + pending.have, in + UI_WIRE_APPEND_TEXT_SIZE, chunk);
        pending.have += chunk;
        if (pending.have == pending.total) put_newer_text(out);
        return;
    }
    case UI_OP_LOAD_FONT: {
        uint32_t rest = op->payload_size - 8u;
        p = emit(out, op->opcode, 12 + rest + 8);
        memset(p, FILL, 12 + rest + 8);
        memcpy(p, in, 8);
        ui_wire_put_u16(p + 6, 12);
        memcpy(p + 12, in + 8, rest);
        return;
    }
    default:
        p = emit(out, op->opcode, op->payload_size + 8u);
        memset(p, FILL, op->payload_size + 8u);
        memcpy(p, in, op->payload_size);
    }
}

static void scene_init(ui_scene *s, captured *c) {
    ui_scene_init(s, &quiet);
    CHECK(ui_scene_add_root(s, surface) == UI_OK);
    s->timer_fn = on_timer;
    s->timer_user = c;
    s->font_fn = on_font;
    s->font_user = c;
}

static bool same_style(const ui_text_style *a, const ui_text_style *b) {
    return a->family == b->family && a->weight == b->weight && a->style == b->style &&
           a->size == b->size && a->language == b->language &&
           !memcmp(a->color, b->color, sizeof a->color);
}

static bool same_node(const ui_node *a, const ui_node *b) {
    if (memcmp(a->rect, b->rect, sizeof a->rect) || memcmp(a->radius, b->radius, sizeof a->radius) ||
        memcmp(a->color, b->color, sizeof a->color) || a->clip != b->clip)
        return false;
    if (!a->anim != !b->anim) return false;
    for (int i = 0; a->anim && i < UI_SPRING_COUNT; i++) {
        const ui_spring *x = &a->anim->springs[i], *y = &b->anim->springs[i];
        if (x->active != y->active ||
            (x->active && (x->stiffness != y->stiffness || x->damping != y->damping ||
                           memcmp(x->target, y->target, sizeof x->target))))
            return false;
    }
    const ui_paragraph *p = a->text, *q = b->text;
    if (!p || !q) return p == q;
    if (p->len != q->len || memcmp(p->text, q->text, p->len) || !same_style(&p->base, &q->base) ||
        p->span_count != q->span_count || p->family_count != q->family_count ||
        memcmp(p->family_offsets, q->family_offsets,
               (p->family_count + 1) * sizeof *p->family_offsets) ||
        memcmp(p->families, q->families, p->family_offsets[p->family_count]) || p->dir != q->dir ||
        p->max_lines != q->max_lines || p->ellipsis_len != q->ellipsis_len ||
        memcmp(p->ellipsis, q->ellipsis, p->ellipsis_len) || p->align != q->align ||
        p->line_height != q->line_height)
        return false;
    for (uint32_t i = 0; i < p->span_count; i++) {
        if (p->spans[i].start != q->spans[i].start || p->spans[i].end != q->spans[i].end ||
            !same_style(&p->spans[i].style, &q->spans[i].style))
            return false;
    }
    return true;
}

// Applies this writer's buffer and the rewritten one to two scenes, which must end up equal.
static void check_same_result(bool paragraph_fields, rewrite_fn fn) {
    char *text = malloc(TEXT_LEN);
    for (uint32_t i = 0; i < TEXT_LEN; i++) text[i] = (char)('a' + i % 26);
    size_t size, other_size;
    uint8_t *bytes = write_all(paragraph_fields, text, &size);
    uint8_t *other = rewrite(bytes, size, fn, &other_size);
    CHECK(other_size != size); // the rewrite changed the bytes
    ui_scene a, b;
    captured ca = {0}, cb = {0};
    scene_init(&a, &ca);
    scene_init(&b, &cb);
    CHECK(ui_scene_apply(&a, bytes, size, 0) == UI_OK);
    CHECK(ui_scene_apply(&b, other, other_size, 0) == UI_OK);
    ui_scene_resolve(&a, 0);
    ui_scene_resolve(&b, 0);
    CHECK_EQ_INT(a.stats.dropped, 0);
    CHECK_EQ_INT(b.stats.dropped, 0);
    CHECK_EQ_INT(b.stats.applied, a.stats.applied);
    CHECK(same_node(&a.nodes[1], &b.nodes[1]));
    CHECK(same_node(&a.nodes[2], &b.nodes[2]));
    const ui_paragraph *t = b.nodes[2].text;
    CHECK(t && t->len == TEXT_LEN && t->span_count == 2); // the continuations arrived
    if (t) {
        CHECK_EQ_INT(t->align, paragraph_fields ? UI_ALIGN_JUSTIFY : UI_ALIGN_START);
        CHECK_NEAR(t->line_height, paragraph_fields ? 1.4 : 0, 1e-6);
        CHECK_EQ_INT(t->spans[0].style.language, 3); // "ro", after the two families
    }
    CHECK(b.nodes[1].clip && b.nodes[2].anim && b.nodes[2].anim->springs[UI_SPRING_RECT].active);
    CHECK(cb.timer_id == 9 && cb.deadline == 123456789);
    CHECK(cb.font_id == 4 && cb.font_len == 16 && !memcmp(cb.font_path, "/fonts/Extra.ttf", 16));
    free(bytes);
    free(other);
    free(text);
    ui_scene_destroy(&a);
    ui_scene_destroy(&b);
}

static void test_older_writer(void) { check_same_result(false, as_older); }

static void test_newer_writer(void) {
    check_same_result(true, as_newer);
    check_same_result(false, as_newer);
}

// Applies a buffer holding one op with the given payload to a fresh scene.
static ui_result apply_patched(uint16_t opcode, const uint8_t *payload, uint32_t size) {
    ui_wire_buf b;
    ui_wire_buf_init(&b, UI_WIRE_MAGIC_CMD);
    memcpy(emit(&b, opcode, size), payload, size);
    size_t n;
    uint8_t *bytes = ui_wire_buf_finish(&b, &n);
    ui_scene s;
    captured c = {0};
    scene_init(&s, &c);
    ui_result r = ui_scene_apply(&s, bytes, n, 0);
    free(bytes);
    ui_scene_destroy(&s);
    return r;
}

static void test_too_short(void) {
    // Below an op's original size, the buffer is rejected.
    uint8_t zeros[256] = {0};
    CHECK(apply_patched(UI_OP_SET_RECT, zeros, UI_WIRE_SET_RECT_SIZE - 4) == UI_ERR_MALFORMED);
    CHECK(apply_patched(UI_OP_SET_RECT, zeros, UI_WIRE_SET_RECT_SIZE) == UI_OK); // dropped
    CHECK(apply_patched(UI_OP_SET_TEXT, zeros, 60) == UI_ERR_MALFORMED);

    // set_text sizes that are below the original or not a multiple of 4.
    char *text = malloc(TEXT_LEN);
    memset(text, 'x', TEXT_LEN);
    size_t size;
    uint8_t *bytes = write_all(false, text, &size);
    ui_wire_reader r;
    ui_wire_reader_init(&r, UI_WIRE_MAGIC_CMD, bytes, size);
    ui_wire_op op;
    while (ui_wire_reader_next(&r, &op) == UI_OK && op.opcode != UI_OP_SET_TEXT) {}
    uint8_t *p = malloc(op.payload_size);
    const struct {
        uint32_t offset, value;
        ui_result want;
    } cases[] = {
        {14, 72, UI_OK},            // as written
        {14, 60, UI_ERR_MALFORMED}, // header below 64
        {14, 70, UI_ERR_MALFORMED}, // header not a multiple of 4
        {14, 2000, UI_ERR_MALFORMED}, // header past the payload
        {28, 32, UI_ERR_MALFORMED}, // spans below 36
        {28, 38, UI_ERR_MALFORMED}, // spans not a multiple of 4
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        memcpy(p, op.payload, op.payload_size);
        ui_wire_put_u16(p + cases[i].offset, (uint16_t)cases[i].value);
        ui_wire_put_u32(p + 8, ui_wire_get_u16(p + 12)); // no continuations in this buffer
        CHECK_EQ_INT(apply_patched(UI_OP_SET_TEXT, p, op.payload_size), cases[i].want);
    }
    free(p);
    free(bytes);
    free(text);
}

static void test_unknown_ops(void) {
    // Opcodes from a newer writer are dropped whatever their size, even shorter than a
    // handle and at the very end of the buffer (ASan checks nothing past them is read), and
    // the ops around them still apply.
    ui_wire_buf b;
    ui_wire_buf_init(&b, UI_WIRE_MAGIC_CMD);
    emit(&b, 200, 0);
    uint8_t *p = emit(&b, UI_OP_CREATE_NODE, UI_WIRE_CREATE_NODE_SIZE);
    ui_wire_put_handle(ui_wire_put_handle(p, node), surface);
    memset(emit(&b, 201, 4), FILL, 4);
    size_t n;
    uint8_t *bytes = ui_wire_buf_finish(&b, &n);
    uint8_t *exact = malloc(n); // no slack after the last op
    memcpy(exact, bytes, n);
    ui_scene s;
    captured c = {0};
    scene_init(&s, &c);
    CHECK(ui_scene_apply(&s, exact, n, 0) == UI_OK);
    CHECK_EQ_INT(s.stats.applied, 1);
    CHECK_EQ_INT(s.stats.dropped, 2);
    ui_scene_destroy(&s);
    free(exact);
    free(bytes);
}

// start_spring on a property from a newer writer (with no components, or some) is dropped;
// so is a negative radius target. Nothing becomes the node's radius (once, an unknown
// property with count 0 set the radius from a target never read).
static void test_start_spring_unknown(void) {
    ui_wire_buf b;
    ui_wire_buf_init(&b, UI_WIRE_MAGIC_CMD);
    uint8_t *p = emit(&b, UI_OP_CREATE_NODE, UI_WIRE_CREATE_NODE_SIZE);
    ui_wire_put_handle(ui_wire_put_handle(p, node), surface);
    const struct { uint16_t property, count; float target; } cases[] = {{77, 0, 0}, {77, 1, 5}, {UI_PROP_CORNER_RADIUS, 1, -3}};
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        p = emit(&b, UI_OP_START_SPRING, UI_WIRE_START_SPRING_SIZE + 4u * cases[i].count);
        p = ui_wire_put_handle(p, node);
        p = ui_wire_put_u16(p, cases[i].property);
        p = ui_wire_put_u16(p, cases[i].count);
        p = ui_wire_put_f32(p, 100);
        p = ui_wire_put_f32(p, 10);
        if (cases[i].count) ui_wire_put_f32(p, cases[i].target);
    }
    size_t n;
    uint8_t *bytes = ui_wire_buf_finish(&b, &n);
    ui_scene s;
    captured c = {0};
    scene_init(&s, &c);
    CHECK(ui_scene_apply(&s, bytes, n, 0) == UI_OK);
    CHECK_EQ_INT(s.stats.applied, 1);
    CHECK_EQ_INT(s.stats.dropped, 3);
    ui_scene_resolve(&s, 0);
    const ui_node *nd = &s.nodes[ui_handle_slot(node)];
    CHECK(nd->override_count == 0 && nd->computed.corner_radius[0] == 0 && nd->radius[0] == 0);
    CHECK(ui_scene_check_resolved(&s));
    ui_scene_destroy(&s);
    free(bytes);
}

// Styles from a newer writer: unknown properties and kinds are skipped, longer entries and
// headers are read for the fields this reader knows. Framing errors reject the buffer.
static void test_styles(void) {
    uint8_t define[] = {
        0x78, 0x56, 0x34, 0x12, 12, 0, 2, 0, 0xEE, 0xEE, 0xEE, 0xEE, // header 12, 2 blocks
        // 40 bytes, 3 entries, a 16-byte header: this version's 12 (no token declarations),
        // then a newer writer's 4.
        40, 0, UI_STATE_HOVER, 0, 3, 0, 16, 0, 0, 0, 0, 0, 0xEE, 0xEE, 0xEE, 0xEE,
        0xE7, 3, 2, 2, 0, 0, 0x80, 0x3F,                             // unknown property 999
        1, 0, 77, 1,                                                 // unknown kind 77
        5, 0, 6, 3, 0, 0, 0, 0x3F, 0xEE, 0xEE, 0xEE, 0xEE,            // opacity 0.5 + a word
        8, 0, 0, 0, 0, 0, 0, 0,                                      // an empty base block
    };
    ui_wire_buf b;
    ui_wire_buf_init(&b, UI_WIRE_MAGIC_CMD);
    memcpy(emit(&b, UI_OP_DEFINE_STYLE, sizeof define), define, sizeof define);
    uint8_t *p = emit(&b, UI_OP_CREATE_NODE, UI_WIRE_CREATE_NODE_SIZE);
    ui_wire_put_handle(ui_wire_put_handle(p, node), surface);
    // set_props with a 16-byte header and an unknown entry before a known one.
    p = emit(&b, UI_OP_SET_PROPS, 16 + 4 + 8);
    p = ui_wire_put_handle(p, node);
    p = ui_wire_put_u16(p, 16);
    p = ui_wire_put_u16(p, 2);
    p = ui_wire_put_u32(p, 0xEEEEEEEEu);
    p = ui_wire_put_u32(p, 0x0101B000u); // property 0xB000 (unknown transition), kind 1, 1 word
    p = ui_wire_put_u16(p, UI_STYLE_BORDER_WIDTH);
    *p++ = UI_VALUE_PX;
    *p++ = 2;
    ui_wire_put_f32(p, 3);
    // set_token with bytes appended after its entry.
    p = emit(&b, UI_OP_SET_TOKEN, 4 + 8 + 4);
    p = ui_wire_put_u32(p, 9);
    p = ui_wire_put_u16(p, 0);
    *p++ = UI_VALUE_NUMBER;
    *p++ = 2;
    p = ui_wire_put_f32(p, 0.25f);
    ui_wire_put_u32(p, 0xEEEEEEEEu);
    // set_styles with a 16-byte header.
    p = emit(&b, UI_OP_SET_STYLES, 16 + 4);
    p = ui_wire_put_handle(p, node);
    p = ui_wire_put_u16(p, 1);
    p = ui_wire_put_u16(p, 16);
    p = ui_wire_put_u32(p, 0xEEEEEEEEu);
    ui_wire_put_u32(p, 0x12345678u);
    size_t n;
    uint8_t *bytes = ui_wire_buf_finish(&b, &n);
    ui_scene s;
    captured c = {0};
    scene_init(&s, &c);
    CHECK(ui_scene_apply(&s, bytes, n, 0) == UI_OK);
    CHECK_EQ_INT(s.stats.dropped, 0);
    CHECK_EQ_INT(s.stats.applied, 5);
    ui_scene_resolve(&s, 0);
    const ui_style_def *d = ui_styles_find(&s.styles, 0x12345678);
    CHECK(d && d->block_count == 2 && d->entry_count == 1);
    if (d && d->entry_count) CHECK(d->entries[0].prop == UI_STYLE_OPACITY && d->entries[0].value.number == 0.5f);
    const ui_node *nd = &s.nodes[ui_handle_slot(node)];
    CHECK(nd->border_width[0] == 3 && nd->border_width[3] == 3 && nd->override_count == 1);
    CHECK(nd->opacity == 1); // the hover block doesn't match
    free(bytes);
    ui_scene_destroy(&s);

    // Framing errors: an entry of 0 words, a block past the payload, a block header below 8,
    // token declarations past the block; set_props entries past the payload, set_styles ids
    // past it, a token's entry past it.
    uint8_t bad[sizeof define];
    const struct {
        uint32_t offset;
        uint8_t value;
    } patches[] = {{31, 0}, {12, 200}, {18, 4}, {20, 1}};
    for (size_t i = 0; i < sizeof patches / sizeof *patches; i++) {
        memcpy(bad, define, sizeof bad);
        bad[patches[i].offset] = patches[i].value;
        CHECK_EQ_INT(apply_patched(UI_OP_DEFINE_STYLE, bad, sizeof bad), UI_ERR_MALFORMED);
    }
    uint8_t props[16] = {0};
    ui_wire_put_u16(props + 8, 12);
    ui_wire_put_u16(props + 10, 2); // two entries, room for one
    props[15] = 1;
    CHECK_EQ_INT(apply_patched(UI_OP_SET_PROPS, props, sizeof props), UI_ERR_MALFORMED);
    uint8_t styles[16] = {0};
    ui_wire_put_u16(styles + 8, 2); // two ids, room for one
    CHECK_EQ_INT(apply_patched(UI_OP_SET_STYLES, styles, sizeof styles), UI_ERR_MALFORMED);
    uint8_t token[8] = {1, 0, 0, 0, 0, 0, UI_VALUE_COLOR, 5}; // a color needs 20 bytes
    CHECK_EQ_INT(apply_patched(UI_OP_SET_TOKEN, token, sizeof token), UI_ERR_MALFORMED);
    // Well-framed but invalid values drop just the op.
    uint8_t negative[16] = {0};
    ui_wire_put_u32(negative, 3);
    negative[6] = UI_VALUE_PX;
    negative[7] = 2;
    ui_wire_put_f32(negative + 8, -1); // a token may hold any length; a border ignores it
    CHECK_EQ_INT(apply_patched(UI_OP_SET_TOKEN, negative, 12), UI_OK);
    // A string longer than its entry drops the op (it is never read past the entry).
    uint8_t string[20] = {0};
    ui_wire_put_handle(string, node);
    ui_wire_put_u16(string + 8, 12);
    ui_wire_put_u16(string + 10, 1);
    ui_wire_put_u16(string + 12, UI_STYLE_FONT_FAMILY);
    string[14] = UI_VALUE_STRING;
    string[15] = 2;
    ui_wire_put_u16(string + 16, 3); // 3 bytes, room for 2
    string[18] = 'a';
    string[19] = 'b';
    ui_scene sc;
    captured cs = {0};
    scene_init(&sc, &cs);
    ui_wire_buf_init(&b, UI_WIRE_MAGIC_CMD);
    p = emit(&b, UI_OP_CREATE_NODE, UI_WIRE_CREATE_NODE_SIZE);
    ui_wire_put_handle(ui_wire_put_handle(p, node), surface);
    memcpy(emit(&b, UI_OP_SET_PROPS, sizeof string), string, sizeof string);
    string[16] = 2; // now it fits: applied
    memcpy(emit(&b, UI_OP_SET_PROPS, sizeof string), string, sizeof string);
    bytes = ui_wire_buf_finish(&b, &n);
    CHECK(ui_scene_apply(&sc, bytes, n, 0) == UI_OK);
    CHECK_EQ_INT(sc.stats.dropped, 1);
    ui_scene_resolve(&sc, 0);
    const char *fam = sc.nodes[ui_handle_slot(node)].computed.text.family;
    CHECK(fam && !strcmp(fam, "ab"));
    free(bytes);
    ui_scene_destroy(&sc);
}

static uint8_t *put_rgba(uint8_t *p, float r, float g, float b, float a) {
    return ui_wire_put_f32(ui_wire_put_f32(ui_wire_put_f32(ui_wire_put_f32(p, r), g), b), a);
}

// Token declarations from a newer writer (docs/styles.md, "Token scopes"): a longer block
// header, declarations of kinds this reader doesn't know (skipped), longer entries, bytes
// after them in the block; set_token_overrides with a longer header and a longer entry.
static void test_token_scopes(void) {
    uint8_t define[] = {
        0x77, 0, 0, 0, 8, 0, 1, 0,                                    // style 0x77, 1 block
        64, 0, 0, 0, 1, 0, 16, 0, 2, 0, 0, 0, 0xEE, 0xEE, 0xEE, 0xEE, // 64 bytes, header 16, 2 tokens
        1, 0, UI_VALUE_TOKEN, 2, 1, 0, 0, 0,                          // background: token 1
        1, 0, 0, 0, 2, 0, 0, 0,                                       // tokens 1 and 2
        0, 0, UI_VALUE_COLOR, 6, 0, 0, 0, 0, 0, 0, 0x80, 0x3F,        // 1: green and a word
        0, 0, 0, 0, 0, 0, 0x80, 0x3F, 0xEE, 0xEE, 0xEE, 0xEE,
        0, 0, 77, 1,                                                  // 2: a kind from the future
        0xEE, 0xEE, 0xEE, 0xEE,                                       // and more in the block
    };
    ui_wire_buf b;
    ui_wire_buf_init(&b, UI_WIRE_MAGIC_CMD);
    memcpy(emit(&b, UI_OP_DEFINE_STYLE, sizeof define), define, sizeof define);
    uint8_t *p = emit(&b, UI_OP_CREATE_NODE, UI_WIRE_CREATE_NODE_SIZE);
    ui_wire_put_handle(ui_wire_put_handle(p, node), surface);
    p = emit(&b, UI_OP_CREATE_NODE, UI_WIRE_CREATE_NODE_SIZE);
    ui_wire_put_handle(ui_wire_put_handle(p, child), node);
    p = emit(&b, UI_OP_SET_STYLES, 16);
    p = ui_wire_put_u32(ui_wire_put_u16(ui_wire_put_u16(ui_wire_put_handle(p, node), 1), 12), 0x77);
    p = emit(&b, UI_OP_SET_STYLES, 16);
    ui_wire_put_u32(ui_wire_put_u16(ui_wire_put_u16(ui_wire_put_handle(p, child), 1), 12), 0x77);
    p = emit(&b, UI_OP_SET_TOKEN, 4 + 20);
    put_rgba(ui_wire_put_u32(ui_wire_put_u32(p, 1), 0x05070000u), 1, 0, 0, 1); // red
    // The child's own: header 16, token 3 of an unknown kind, token 1 blue with a word more.
    p = emit(&b, UI_OP_SET_TOKEN_OVERRIDES, 16 + 8 + 4 + 24);
    p = ui_wire_put_u32(ui_wire_put_u16(ui_wire_put_u16(ui_wire_put_handle(p, child), 16), 2), 0xEEEEEEEEu);
    p = ui_wire_put_u32(ui_wire_put_u32(p, 3), 1);
    p = ui_wire_put_u32(p, 0x01580000u); // kind 88, 1 word
    p = put_rgba(ui_wire_put_u32(p, 0x06070000u), 0, 0, 1, 1);
    ui_wire_put_u32(p, 0xEEEEEEEEu);
    size_t n;
    uint8_t *bytes = ui_wire_buf_finish(&b, &n);
    ui_scene s;
    captured c = {0};
    scene_init(&s, &c);
    CHECK(ui_scene_apply(&s, bytes, n, 0) == UI_OK);
    CHECK_EQ_INT(s.stats.dropped, 0);
    ui_scene_resolve(&s, 0);
    CHECK(ui_scene_check_resolved(&s));
    const ui_style_def *d = ui_styles_find(&s.styles, 0x77);
    CHECK(d && d->entry_count == 1 && d->decl_count == 1 && d->decls[0].token == 1);
    const float *green = s.nodes[ui_handle_slot(node)].computed.background;
    const float *blue = s.nodes[ui_handle_slot(child)].computed.background;
    CHECK(green[0] == 0 && green[1] == 1 && green[2] == 0);
    CHECK(blue[0] == 0 && blue[1] == 0 && blue[2] == 1);
    free(bytes);
    ui_scene_destroy(&s);
    // Framing: declarations past the op.
    uint8_t short_op[24] = {0};
    ui_wire_put_u16(short_op + 8, 12);
    ui_wire_put_u16(short_op + 10, 2); // two tokens: ids fit, their entries don't
    CHECK_EQ_INT(apply_patched(UI_OP_SET_TOKEN_OVERRIDES, short_op, sizeof short_op), UI_ERR_MALFORMED);
    // Invalid values drop the op: a 0 token, a value a token can't hold, unset in a style.
    uint8_t zero[24] = {0};
    ui_wire_put_u16(zero + 8, 12);
    ui_wire_put_u16(zero + 10, 1);
    ui_wire_put_u32(zero + 16, 0x01010000u); // token 0, unset
    ui_scene z;
    scene_init(&z, &c);
    ui_wire_buf_init(&b, UI_WIRE_MAGIC_CMD);
    p = emit(&b, UI_OP_CREATE_NODE, UI_WIRE_CREATE_NODE_SIZE);
    ui_wire_put_handle(ui_wire_put_handle(p, node), surface);
    memcpy(emit(&b, UI_OP_SET_TOKEN_OVERRIDES, sizeof zero), zero, sizeof zero);
    ui_wire_put_handle(b.data + b.size - sizeof zero, node);
    bytes = ui_wire_buf_finish(&b, &n);
    CHECK(ui_scene_apply(&z, bytes, n, 0) == UI_OK);
    CHECK_EQ_INT(z.stats.dropped, 1);
    free(bytes);
    ui_scene_destroy(&z);
}

static void test_events(void) {
    // A newer core appended fields to frame_committed, timer_fired and layout_measured: they
    // are skipped, and the events after them still read correctly.
    ui_wire_buf b;
    ui_wire_buf_init(&b, UI_WIRE_MAGIC_EVENT);
    uint8_t *p = emit(&b, UI_EVENT_FRAME_COMMITTED, UI_WIRE_FRAME_COMMITTED_SIZE + 8);
    memset(p, FILL, UI_WIRE_FRAME_COMMITTED_SIZE + 8);
    p = ui_wire_put_handle(p, surface);
    p = ui_wire_put_u64(p, 42);
    ui_wire_put_u64(p, 1000);
    p = emit(&b, UI_EVENT_TIMER_FIRED, UI_WIRE_TIMER_FIRED_SIZE + 4);
    memset(p, FILL, UI_WIRE_TIMER_FIRED_SIZE + 4);
    p = ui_wire_put_u32(p, 7);
    p = ui_wire_put_u32(p, 0);
    p = ui_wire_put_u64(p, 5);
    ui_wire_put_u64(p, 6);
    CHECK(ui_wire_surface_close_requested_put(&b, &(ui_wire_surface_close_requested){surface}) == UI_OK);
    p = emit(&b, UI_EVENT_LAYOUT_MEASURED, UI_WIRE_LAYOUT_MEASURED_SIZE + 12);
    memset(p, FILL, UI_WIRE_LAYOUT_MEASURED_SIZE + 12);
    p = ui_wire_put_u32(p, 9);
    p = ui_wire_put_u32(p, (uint32_t)UI_OK);
    p = ui_wire_put_handle(p, node);
    p = ui_wire_put_handle(p, surface);
    for (int i = 0; i < 16; i++) p = ui_wire_put_f32(p, (float)i);
    size_t n;
    uint8_t *bytes = ui_wire_buf_finish(&b, &n);
    ui_event_reader rd;
    ui_event ev;
    CHECK(ui_event_reader_init(&rd, bytes, n) == UI_OK);
    CHECK(ui_event_next(&rd, &ev) == UI_OK && ev.type == UI_EVENT_FRAME_COMMITTED);
    CHECK(ev.frame_committed.frame_id == 42 && ev.frame_committed.predicted_present_ns == 1000);
    CHECK(ui_event_next(&rd, &ev) == UI_OK && ev.type == UI_EVENT_TIMER_FIRED);
    CHECK(ev.timer_fired.id == 7 && ev.timer_fired.deadline_ns == 5 && ev.timer_fired.now_ns == 6);
    CHECK(ui_event_next(&rd, &ev) == UI_OK && ev.type == UI_EVENT_SURFACE_CLOSE_REQUESTED);
    CHECK(ui_event_next(&rd, &ev) == UI_OK && ev.type == UI_EVENT_LAYOUT_MEASURED);
    CHECK(ev.layout_measured.request_id == 9 && ui_handle_eq(ev.layout_measured.node, node) &&
          ev.layout_measured.presentation_in_surface.height == 15);
    CHECK(ui_event_next(&rd, &ev) == UI_ERR_END);
    free(bytes);

    // Shorter than the original size is malformed.
    ui_wire_buf_init(&b, UI_WIRE_MAGIC_EVENT);
    const uint32_t short_size = UI_WIRE_TIMER_FIRED_SIZE - 8;
    memset(emit(&b, UI_EVENT_TIMER_FIRED, short_size), 0, short_size);
    bytes = ui_wire_buf_finish(&b, &n);
    CHECK(ui_event_reader_init(&rd, bytes, n) == UI_OK);
    CHECK(ui_event_next(&rd, &ev) == UI_ERR_MALFORMED);
    free(bytes);
}

int main(void) {
    RUN(test_older_writer);
    RUN(test_newer_writer);
    RUN(test_too_short);
    RUN(test_styles);
    RUN(test_unknown_ops);
    RUN(test_start_spring_unknown);
    RUN(test_token_scopes);
    RUN(test_events);
    return test_failures != 0;
}
