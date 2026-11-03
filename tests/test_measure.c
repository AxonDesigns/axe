// Measurement: request_layout answered by a layout_measured event with the layout and
// presentation rects, each relative to the parent and to the surface. Scene-level checks of
// the rects (nested, animating, hidden, stale), the event's encoding, and an app under both
// schedulers: the answer comes in the frame that applies the request, a stale handle is
// answered as such, and a request on an idle surface is answered without drawing a frame.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "core/scene.h"
#include "test.h"
#include "ui/ui.h"
#include "wire/gen/wire_gen.h"

static ui_logger quiet = {UI_LOG_ERROR};

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

static void frame(fixture *f) {
    uint8_t *bytes;
    size_t sz;
    if (ui_cmd_writer_op_count(f->w)) {
        ui_cmd_writer_take(f->w, &bytes, &sz);
        CHECK(ui_scene_apply(&f->s, bytes, sz, f->now) == UI_OK);
        free(bytes);
    }
    ui_scene_resolve(&f->s, f->now);
    ui_scene_animate(&f->s, f->now);
    ui_scene_layout(&f->s, f->surface, 400, 300, 1, NULL, f->now);
}

static bool rect_is(const float *r, float x, float y, float w, float h) {
    bool ok = fabsf(r[0] - x) < 1e-4f && fabsf(r[1] - y) < 1e-4f && fabsf(r[2] - w) < 1e-4f &&
              fabsf(r[3] - h) < 1e-4f;
    if (!ok)
        fprintf(stderr, "    got %g %g %g %g, want %g %g %g %g\n", (double)r[0], (double)r[1], (double)r[2],
                (double)r[3], (double)x, (double)y, (double)w, (double)h);
    return ok;
}

// The surface (padding 10 on node 1) holds P (2: margin-left 5, padding 3), which holds N
// (3: 20 x 10, with a layout transition).
static void test_rects(void) {
    fixture f = {0};
    ui_scene_init(&f.s, &quiet);
    f.surface = (ui_handle){(uint32_t)UI_HANDLE_SURFACE << UI_HANDLE_SLOT_BITS, 1};
    ui_scene_add_root(&f.s, f.surface);
    ui_cmd_writer_create(&f.w);
    ui_cmd_create_node(f.w, node(1), f.surface);
    props(&f, 1, (ui_style_decl[]){{UI_STYLE_PADDING_TOP, ui_px(10)}, {UI_STYLE_PADDING_LEFT, ui_px(10)},
                                   {UI_STYLE_ALIGN_ITEMS, ui_keyword(UI_FLEX_START)}}, 3);
    ui_cmd_create_node(f.w, node(2), node(1));
    props(&f, 2, (ui_style_decl[]){{UI_STYLE_MARGIN_LEFT, ui_px(5)}, {UI_STYLE_PADDING_TOP, ui_px(3)},
                                   {UI_STYLE_PADDING_LEFT, ui_px(3)}}, 3);
    ui_cmd_create_node(f.w, node(3), node(2));
    props(&f, 3, (ui_style_decl[]){{UI_STYLE_WIDTH, ui_px(20)}, {UI_STYLE_HEIGHT, ui_px(10)},
                                   {UI_STYLE_TRANSITION(UI_STYLE_LAYOUT), ui_transition(100, 20)}}, 3);
    frame(&f);
    float r[4][4];
    ui_handle surface;
    CHECK(ui_scene_measure(&f.s, node(3), r, &surface) && ui_handle_eq(surface, f.surface));
    CHECK(rect_is(r[0], 3, 3, 20, 10));   // layout, in P
    CHECK(rect_is(r[1], 18, 13, 20, 10)); // layout, on the surface: 10 + 5 + 3, 10 + 3
    CHECK(!memcmp(r[0], r[2], sizeof r[0]) && !memcmp(r[1], r[3], sizeof r[1])); // at rest
    // Mid-animation the presentation lags the layout, which is final at once.
    props(&f, 2, &(ui_style_decl){UI_STYLE_PADDING_LEFT, ui_px(43)}, 1);
    frame(&f);
    f.now += 50 * MS;
    frame(&f);
    CHECK(ui_scene_measure(&f.s, node(3), r, &surface));
    CHECK(rect_is(r[0], 43, 3, 20, 10) && rect_is(r[1], 58, 13, 20, 10));
    CHECK(r[2][0] > 3 && r[2][0] < 43 && r[2][1] == 3);
    CHECK(r[3][0] == r[2][0] + 15 && r[3][1] == 13);
    // Under a hidden node: all zero. A stale handle: false, zero, no surface.
    props(&f, 2, &(ui_style_decl){UI_STYLE_DISPLAY, ui_keyword(UI_DISPLAY_NONE)}, 1);
    frame(&f);
    static const float zero[4][4];
    CHECK(ui_scene_measure(&f.s, node(3), r, &surface) && !memcmp(r, zero, sizeof r));
    ui_cmd_destroy_node(f.w, node(2));
    frame(&f);
    CHECK(!ui_scene_measure(&f.s, node(3), r, &surface) && !memcmp(r, zero, sizeof r));
    CHECK(ui_handle_is_null(surface));
    CHECK(!ui_scene_measure(&f.s, f.surface, r, &surface)); // not a node
    ui_cmd_writer_destroy(f.w);
    ui_scene_destroy(&f.s);
}

// The op's framing, and the event's encoding through the public reader.
static void test_wire(void) {
    ui_cmd_writer *w;
    ui_cmd_writer_create(&w);
    CHECK(ui_cmd_request_layout(w, node(4), 77) == UI_OK);
    uint8_t *bytes;
    size_t size;
    ui_cmd_writer_take(w, &bytes, &size);
    CHECK(size == UI_WIRE_HEADER_SIZE + UI_WIRE_OP_HEADER_SIZE + UI_WIRE_REQUEST_LAYOUT_SIZE);
    CHECK(ui_scene_validate(bytes, size) == UI_OK);
    bytes[UI_WIRE_HEADER_SIZE + 2] = 4 + 8; // too short for its opcode: the buffer is rejected
    CHECK(ui_scene_validate(bytes, size) == UI_ERR_MALFORMED);
    free(bytes);
    ui_cmd_writer_destroy(w);

    ui_wire_buf b;
    ui_wire_buf_init(&b, UI_WIRE_MAGIC_EVENT);
    ui_handle surface = {(uint32_t)UI_HANDLE_SURFACE << UI_HANDLE_SLOT_BITS, 3};
    ui_wire_layout_measured m = {.request_id = 77, .result = UI_OK, .node = node(4), .surface = surface};
    ui_wire_rect *rects[4] = {&m.layout_in_parent, &m.layout_in_surface, &m.presentation_in_parent,
                              &m.presentation_in_surface};
    for (int i = 0; i < 4; i++)
        *rects[i] = (ui_wire_rect){4.0f * (float)i + 0.5f, 4.0f * (float)i + 1.5f, 4.0f * (float)i + 2.5f,
                                   4.0f * (float)i + 3.5f};
    CHECK(ui_wire_layout_measured_put(&b, &m) == UI_OK);
    uint8_t *ev = ui_wire_buf_finish(&b, &size);
    ui_event_reader rd;
    ui_event e;
    CHECK(ui_event_reader_init(&rd, ev, size) == UI_OK && ui_event_next(&rd, &e) == UI_OK);
    CHECK(e.type == UI_EVENT_LAYOUT_MEASURED && e.layout_measured.request_id == 77);
    CHECK(ui_handle_eq(e.layout_measured.node, node(4)) && ui_handle_eq(e.surface, surface));
    CHECK(e.layout_measured.result == UI_OK);
    CHECK(e.layout_measured.layout_in_parent.x == 0.5f && e.layout_measured.layout_in_surface.y == 5.5f);
    CHECK(e.layout_measured.presentation_in_parent.width == 10.5f &&
          e.layout_measured.presentation_in_surface.height == 15.5f);
    free(ev);
    ui_wire_buf_free(&b);
}

// ---- Both schedulers ---------------------------------------------------------------------

#define WIDTH 200u
#define HEIGHT 100u

typedef struct measure_logic {
    ui_handle_allocator *alloc;
    ui_handle box, gone;
    int frames;            // frame_committed events seen
    int sent_at;           // frames seen when request 1 was sent
    int answered_at;       // and when its answer arrived
    ui_event answers[3];   // requests 1 (live), 2 (stale), 3 (on a timer, nothing changed)
    int answer_count;
    bool built;
} measure_logic;

static ui_result on_events(void *user, const void *events, size_t sz, ui_cmd_writer *w) {
    measure_logic *l = user;
    ui_event_reader rd;
    ui_result r = ui_event_reader_init(&rd, events, sz);
    ui_event ev;
    while (r == UI_OK && ui_event_next(&rd, &ev) == UI_OK) {
        if (ev.type == UI_EVENT_FRAME_COMMITTED) l->frames++;
        if (ev.type == UI_EVENT_SURFACE_RESIZED && !l->built) {
            l->built = true;
            l->sent_at = l->frames;
            ui_handle row;
            ui_handle_alloc(l->alloc, &row);
            ui_handle_alloc(l->alloc, &l->box);
            ui_handle_alloc(l->alloc, &l->gone);
            ui_cmd_create_node(w, row, ev.surface);
            ui_cmd_set_props(w, row, (ui_style_decl[]){{UI_STYLE_PADDING_LEFT, ui_px(12)},
                                                       {UI_STYLE_PADDING_TOP, ui_px(8)},
                                                       {UI_STYLE_JUSTIFY_CONTENT, ui_keyword(UI_FLEX_CENTER)},
                                                       {UI_STYLE_ALIGN_ITEMS, ui_keyword(UI_FLEX_START)}},
                             4);
            ui_cmd_create_node(w, l->box, row);
            ui_cmd_set_props(w, l->box, (ui_style_decl[]){{UI_STYLE_WIDTH, ui_px(50)}, {UI_STYLE_HEIGHT, ui_px(30)}}, 2);
            ui_cmd_set_color(w, l->box, 0.2f, 0.4f, 0.8f, 1);
            ui_cmd_create_node(w, l->gone, row);
            ui_cmd_destroy_node(w, l->gone);
            ui_cmd_request_layout(w, l->box, 1);  // answered once this buffer's frame laid it out
            ui_cmd_request_layout(w, l->gone, 2); // stale by then
            ui_cmd_set_timer(w, 1, 500 * MS);
        } else if (ev.type == UI_EVENT_TIMER_FIRED) {
            ui_cmd_request_layout(w, l->box, 3); // nothing changed: answered without a frame
        } else if (ev.type == UI_EVENT_LAYOUT_MEASURED && l->answer_count < 3) {
            if (ev.layout_measured.request_id == 1) l->answered_at = l->frames;
            l->answers[l->answer_count++] = ev;
        }
    }
    return UI_OK;
}

static bool run(ui_scheduler_kind scheduler, measure_logic *logic, uint64_t *frames_before_timer,
                uint64_t *frames_after_timer) {
    if (ui_handle_allocator_create(&logic->alloc) != UI_OK) return false;
    ui_app_desc desc = {
        .scheduler = scheduler,
        .gpu = UI_GPU_SOFTWARE,
        .surface = {.kind = UI_SURFACE_HEADLESS, .width = WIDTH, .height = HEIGHT, .scale_120 = 150},
        .logic = {.user = logic, .on_events = on_events},
        .start_paused = true,
    };
    ui_app *app;
    ui_result r = ui_app_create(&desc, &app);
    bool ok = r == UI_OK;
    for (int i = 0; ok && i < 4; i++) ok = ui_app_frame(app) == UI_OK;
    ui_app_stats st;
    if (ok) {
        ui_app_get_stats(app, &st);
        *frames_before_timer = st.frames_committed;
        ui_app_clock_step(app, 500 * MS);
        for (int i = 0; ok && i < 3; i++) ok = ui_app_frame(app) == UI_OK;
        ui_app_get_stats(app, &st);
        *frames_after_timer = st.frames_committed;
        CHECK_EQ_INT(st.commands_dropped, 0);
    }
    if (r == UI_OK) ui_app_destroy(app);
    ui_handle_allocator_destroy(logic->alloc);
    return ok;
}

static void test_schedulers(void) {
    measure_logic l[2] = {0};
    uint64_t before[2], after[2];
    bool ok = run(UI_SCHEDULER_SINGLE, &l[0], &before[0], &after[0]) &&
              run(UI_SCHEDULER_THREADED, &l[1], &before[1], &after[1]);
    CHECK(ok);
    if (!ok) return;
    for (int k = 0; k < 2; k++) {
        CHECK_EQ_INT(l[k].answer_count, 3);
        const ui_event *a = &l[k].answers[0];
        CHECK(a->layout_measured.request_id == 1 && a->layout_measured.result == UI_OK);
        CHECK(ui_handle_eq(a->layout_measured.node, l[k].box));
        // Centered in the row's content box: 12 + (200 - 12 - 50) / 2 = 81.
        const ui_rect *p = &a->layout_measured.layout_in_parent;
        CHECK(p->x == 81 && p->y == 8 && p->width == 50 && p->height == 30);
        CHECK(!memcmp(&a->layout_measured.layout_in_parent, &a->layout_measured.presentation_in_surface,
                      sizeof(ui_rect))); // the row sits at the origin, at rest
        // Sent while handling the first frame's events (the resize comes before that frame's
        // frame_committed), applied and answered by the second.
        CHECK_EQ_INT(l[k].sent_at, 0);
        CHECK_EQ_INT(l[k].answered_at, 2);
        CHECK(l[k].answers[1].layout_measured.request_id == 2 &&
              l[k].answers[1].layout_measured.result == UI_ERR_STALE_HANDLE &&
              ui_handle_is_null(l[k].answers[1].surface));
        CHECK(l[k].answers[2].layout_measured.request_id == 3 &&
              l[k].answers[2].layout_measured.layout_in_surface.x == 81);
        CHECK_EQ_INT(after[k], before[k]); // the request drew nothing
    }
    CHECK(!memcmp(&l[0].answers, &l[1].answers, sizeof l[0].answers)); // identical answers
}

int main(void) {
    RUN(test_rects);
    RUN(test_wire);
    RUN(test_schedulers);
    return test_failures != 0;
}
