// Unit tests for the runtime building blocks: handles, wire codec, scene application,
// springs, clock, mailbox and queue.
#include <stdlib.h>
#include <string.h>

#include "core/clock.h"
#include "core/scene.h"
#include "core/spring.h"
#include "runtime/handle_table.h"
#include "runtime/queue.h"
#include "test.h"
#include "wire/wire.h"

static const ui_logger quiet = {UI_LOG_ERROR};

static void test_handle_generations(void) {
    ui_handle_allocator *a;
    CHECK(ui_handle_allocator_create(&a) == UI_OK);
    ui_handle h1, h2, h3;
    CHECK(ui_handle_alloc(a, &h1) == UI_OK);
    CHECK(ui_handle_alloc(a, &h2) == UI_OK);
    CHECK(!ui_handle_eq(h1, h2));
    CHECK(h1.generation != 0);
    CHECK(ui_handle_kind_of(h1) == UI_HANDLE_NODE);
    CHECK(ui_handle_free(a, h1) == UI_OK);
    CHECK(ui_handle_free(a, h1) == UI_ERR_STALE_HANDLE); // double free detected
    CHECK(ui_handle_alloc(a, &h3) == UI_OK);
    CHECK_EQ_INT(ui_handle_slot(h3), ui_handle_slot(h1)); // slot reused...
    CHECK(h3.generation > h1.generation);                 // ...with a newer generation
    ui_handle_allocator_destroy(a);
}

static void test_handle_claim(void) {
    ui_handle_table t;
    ui_handle_table_init(&t, UI_HANDLE_NODE);
    ui_handle h = {5, 3};
    CHECK(ui_handle_table_claim(&t, h) == UI_OK);
    CHECK(ui_handle_table_valid(&t, h));
    CHECK(ui_handle_table_claim(&t, h) == UI_ERR_STALE_HANDLE);           // already live
    CHECK(!ui_handle_table_valid(&t, (ui_handle){5, 2}));                 // old generation
    CHECK(ui_handle_table_release(&t, h) == UI_OK);
    CHECK(ui_handle_table_claim(&t, (ui_handle){5, 3}) == UI_ERR_STALE_HANDLE); // not newer
    CHECK(ui_handle_table_claim(&t, (ui_handle){5, 4}) == UI_OK);
    ui_handle surface = {(uint32_t)UI_HANDLE_SURFACE << UI_HANDLE_SLOT_BITS, 1};
    CHECK(ui_handle_table_claim(&t, surface) == UI_ERR_MALFORMED);        // wrong kind
    CHECK(ui_handle_table_claim(&t, (ui_handle){UI_HANDLE_MAX_SLOTS, 1}) == UI_ERR_MALFORMED);
    CHECK(ui_handle_table_claim(&t, (ui_handle){7, 0}) == UI_ERR_MALFORMED); // null
    ui_handle_table_destroy(&t);
}

static void take(ui_cmd_writer *w, uint8_t **bytes, size_t *size) {
    CHECK(ui_cmd_writer_take(w, bytes, size) == UI_OK);
}

static void test_wire_roundtrip(void) {
    ui_wire_buf b;
    ui_wire_buf_init(&b, UI_WIRE_MAGIC_EVENT);
    ui_handle s = {(uint32_t)UI_HANDLE_SURFACE << UI_HANDLE_SLOT_BITS, 1};
    CHECK(ui_wire_event_surface_resized(&b, s, 320, 200, 150) == UI_OK);
    CHECK(ui_wire_event_frame_committed(&b, s, 42, 123456789012345ull) == UI_OK);
    CHECK(ui_wire_event_surface_close(&b, s) == UI_OK);
    CHECK(ui_wire_event_timer_fired(&b, 9, 2000000000ull, 2000000123ull) == UI_OK);
    // An op from a future version must be skipped by old readers.
    uint8_t *future = ui_wire_buf_op(&b, 999, 8);
    CHECK(future != NULL);
    memset(future, 0xAB, 8);
    size_t size;
    uint8_t *bytes = ui_wire_buf_finish(&b, &size);

    ui_event_reader r;
    ui_event ev;
    CHECK(ui_event_reader_init(&r, bytes, size) == UI_OK);
    CHECK(ui_event_next(&r, &ev) == UI_OK);
    CHECK(ev.type == UI_EVENT_SURFACE_RESIZED && ui_handle_eq(ev.surface, s));
    CHECK_EQ_INT(ev.surface_resized.scale_120, 150);
    CHECK_NEAR(ev.surface_resized.width, 320, 0);
    CHECK(ui_event_next(&r, &ev) == UI_OK);
    CHECK(ev.type == UI_EVENT_FRAME_COMMITTED);
    CHECK_EQ_INT(ev.frame_committed.frame_id, 42);
    CHECK(ev.frame_committed.predicted_present_ns == 123456789012345ull);
    CHECK(ui_event_next(&r, &ev) == UI_OK);
    CHECK(ev.type == UI_EVENT_SURFACE_CLOSE_REQUESTED);
    CHECK(ui_event_next(&r, &ev) == UI_OK);
    CHECK(ev.type == UI_EVENT_TIMER_FIRED && ev.timer_fired.id == 9);
    CHECK(ev.timer_fired.deadline_ns == 2000000000ull && ev.timer_fired.now_ns == 2000000123ull);
    CHECK(ui_event_next(&r, &ev) == UI_ERR_END);

    // Corruptions are rejected, never read past the end.
    bytes[0] ^= 1;
    CHECK(ui_event_reader_init(&r, bytes, size) == UI_ERR_MALFORMED);
    bytes[0] ^= 1;
    CHECK(ui_event_reader_init(&r, bytes, size - 4) == UI_ERR_MALFORMED);
    free(bytes);
}

static void test_writer_validation(void) {
    ui_cmd_writer *w;
    CHECK(ui_cmd_writer_create(&w) == UI_OK);
    ui_handle n = {1, 1};
    CHECK(ui_cmd_set_rect(w, n, 0, 0, -1, 10) == UI_ERR_INVALID_ARGUMENT);
    CHECK(ui_cmd_set_color(w, n, NAN, 0, 0, 1) == UI_ERR_INVALID_ARGUMENT);
    float t[4] = {0};
    CHECK(ui_cmd_start_spring(w, n, (ui_property)77, t, 100, 10) == UI_ERR_INVALID_ARGUMENT);
    CHECK(ui_cmd_start_spring(w, n, UI_PROP_RECT, t, 0, 10) == UI_ERR_INVALID_ARGUMENT);
    CHECK(ui_cmd_set_timer(w, 1, UINT64_MAX) == UI_ERR_INVALID_ARGUMENT); // reserved: cancel
    CHECK_EQ_INT(ui_cmd_writer_op_count(w), 0);
    uint8_t *bytes;
    size_t size;
    CHECK(ui_cmd_writer_take(w, &bytes, &size) == UI_ERR_END);
    ui_cmd_writer_destroy(w);
}

static ui_handle surface_handle(void) {
    return (ui_handle){(uint32_t)UI_HANDLE_SURFACE << UI_HANDLE_SLOT_BITS, 1};
}

static void test_scene_apply_and_stale(void) {
    ui_scene sc;
    ui_scene_init(&sc, &quiet);
    ui_handle surf = surface_handle();
    CHECK(ui_scene_add_root(&sc, surf) == UI_OK);
    ui_handle_allocator *a;
    ui_handle_allocator_create(&a);
    ui_handle parent, child;
    ui_handle_alloc(a, &parent);
    ui_handle_alloc(a, &child);

    ui_cmd_writer *w;
    ui_cmd_writer_create(&w);
    ui_cmd_create_node(w, parent, surf);
    ui_cmd_set_rect(w, parent, 10, 20, 100, 50);
    ui_cmd_set_color(w, parent, 1, 0, 0, 1);
    ui_cmd_create_node(w, child, parent);
    ui_cmd_set_rect(w, child, 5, 5, 10, 10);
    ui_cmd_set_color(w, child, 0, 0, 1, 0.5f);
    uint8_t *bytes;
    size_t size;
    take(w, &bytes, &size);
    CHECK(ui_scene_apply(&sc, bytes, size, 0) == UI_OK);
    ui_scene_resolve(&sc, 0);
    free(bytes);
    CHECK_EQ_INT(sc.stats.applied, 6);
    CHECK_EQ_INT(sc.stats.dropped, 0);

    ui_display_list dl = {.scale_120 = 180}; // 1.5x
    ui_scene_layout(&sc, surf, 400, 300, 1.5f, NULL, 0);
    CHECK(ui_scene_build(&sc, surf, &dl, NULL) == UI_OK);
    CHECK_EQ_INT(dl.rect_count, 2);
    CHECK_NEAR(dl.rects[0].x, 15, 1e-4);         // 10 * 1.5
    CHECK_NEAR(dl.rects[1].x, 23, 1e-4);         // (10 + 5) * 1.5 = 22.5, snapped at rest
    CHECK_NEAR(dl.rects[1].color[3], 0.5, 1e-6);
    CHECK_NEAR(dl.rects[1].color[2], 0.5, 1e-6); // premultiplied

    // Destroying the parent destroys the subtree; later commands to either are dropped.
    ui_cmd_destroy_node(w, parent);
    ui_cmd_set_rect(w, child, 0, 0, 1, 1);
    ui_cmd_set_color(w, parent, 0, 1, 0, 1);
    take(w, &bytes, &size);
    CHECK(ui_scene_apply(&sc, bytes, size, 0) == UI_OK);
    free(bytes);
    CHECK_EQ_INT(sc.stats.applied, 7);
    CHECK_EQ_INT(sc.stats.dropped, 2);
    ui_display_list_reset(&dl);
    ui_scene_layout(&sc, surf, 400, 300, 1.5f, NULL, 0);
    ui_scene_build(&sc, surf, &dl, NULL);
    CHECK_EQ_INT(dl.rect_count, 0);

    // The client frees and reuses the slot: the new generation is accepted, the old is not.
    ui_handle_free(a, parent);
    ui_handle reused;
    ui_handle_alloc(a, &reused);
    CHECK_EQ_INT(ui_handle_slot(reused), ui_handle_slot(parent));
    ui_cmd_create_node(w, reused, surf);
    ui_cmd_set_rect(w, parent, 0, 0, 1, 1); // stale generation of the same slot
    take(w, &bytes, &size);
    ui_scene_apply(&sc, bytes, size, 0);
    free(bytes);
    CHECK_EQ_INT(sc.stats.applied, 8);
    CHECK_EQ_INT(sc.stats.dropped, 3);

    ui_display_list_free(&dl);
    ui_cmd_writer_destroy(w);
    ui_handle_allocator_destroy(a);
    ui_scene_destroy(&sc);
}

static void test_scene_rejects_malformed_atomically(void) {
    ui_scene sc;
    ui_scene_init(&sc, &quiet);
    ui_handle surf = surface_handle();
    ui_scene_add_root(&sc, surf);
    ui_cmd_writer *w;
    ui_cmd_writer_create(&w);
    ui_handle n = {1, 1};
    ui_cmd_create_node(w, n, surf);
    ui_cmd_set_rect(w, n, 0, 0, 10, 10);
    uint8_t *bytes;
    size_t size;
    take(w, &bytes, &size);
    // Corrupt the second op's size: the whole buffer must be rejected, including op 1.
    size_t second = UI_WIRE_HEADER_SIZE + 4 + UI_WIRE_SIZE_CREATE_NODE;
    ui_wire_put_u16(bytes + second + 2, 8);
    CHECK(ui_scene_apply(&sc, bytes, size, 0) == UI_ERR_MALFORMED);
    CHECK_EQ_INT(sc.stats.applied, 0);
    CHECK_EQ_INT(sc.stats.rejected, 1);
    // Truncation and garbage too.
    CHECK(ui_scene_apply(&sc, bytes, size - 4, 0) == UI_ERR_MALFORMED);
    uint8_t junk[64];
    for (size_t i = 0; i < sizeof junk; i++) junk[i] = (uint8_t)(i * 37 + 11);
    CHECK(ui_scene_apply(&sc, junk, sizeof junk, 0) == UI_ERR_MALFORMED);
    free(bytes);
    ui_cmd_writer_destroy(w);
    ui_scene_destroy(&sc);
}

static void test_spring_matches_integration(void) {
    // The closed form must agree with a fine numerical integration, for all three regimes.
    const double cases[][2] = {{170, 18.25}, {100, 20}, {100, 40}};
    for (size_t c = 0; c < 3; c++) {
        double k = cases[c][0], d = cases[c][1];
        double x = 50, v = -30, dt = 1e-5;
        for (int i = 0; i < 30000; i++) { // 0.3 s, semi-implicit Euler
            v += (-k * x - d * v) * dt;
            x += v * dt;
        }
        double ax, av;
        ui_spring_solve(k, d, 50, -30, 0.3, &ax, &av);
        CHECK_NEAR(ax, x, 0.01);
        CHECK_NEAR(av, v, 0.1);
    }
}

static void test_spring_frame_independent_and_retarget(void) {
    float from[1] = {0}, to[1] = {100};
    ui_spring a = {0}, b = {0};
    ui_spring_start(&a, 1, from, to, 170, 18.25, 0);
    ui_spring_start(&b, 1, from, to, 170, 18.25, 0);
    ui_spring_advance(&a, 100000000, 0); // one 100 ms step
    for (uint64_t t = 1; t <= 6; t++) ui_spring_advance(&b, t * 100000000 / 6, 0); // 60 Hz
    CHECK_NEAR(a.position[0], b.position[0], 1e-9);
    CHECK_NEAR(a.velocity[0], b.velocity[0], 1e-9);

    // Retarget mid-flight: position and velocity carry over exactly.
    double pos = a.position[0], vel = a.velocity[0];
    CHECK(vel > 0);
    float back[1] = {0};
    ui_spring_start(&a, 1, from, back, 170, 18.25, 100000000);
    CHECK_NEAR(a.position[0], pos, 1e-9);
    CHECK_NEAR(a.velocity[0], vel, 1e-9);
    CHECK_NEAR(a.target[0], 0, 0);

    // Settles and snaps.
    CHECK(!ui_spring_advance(&a, 200000000, 0.01));
    CHECK(ui_spring_advance(&a, 3000000000ull, 0.01));
    CHECK(!a.active);
    CHECK_NEAR(a.position[0], 0, 0);
}

static uint64_t fake_now;
static uint64_t fake_source(void *user) {
    (void)user;
    return fake_now;
}

static void test_clock_controls(void) {
    fake_now = 1000;
    ui_clock c;
    ui_clock_init(&c, (ui_time_source){fake_source, NULL}, true);
    fake_now = 5000;
    CHECK_EQ_INT(ui_clock_at(&c, fake_now), 0); // paused
    ui_clock_step(&c, 250);
    CHECK_EQ_INT(ui_clock_at(&c, fake_now), 250);
    ui_clock_pause(&c, false);
    fake_now = 6000;
    CHECK_EQ_INT(ui_clock_at(&c, fake_now), 1250);
    ui_clock_set_scale(&c, 0.5);
    fake_now = 8000;
    CHECK_EQ_INT(ui_clock_at(&c, fake_now), 2250);
    CHECK_EQ_INT(ui_clock_at(&c, 10000), 3250); // predicted future times map forward
}

static void test_mailbox_replaces(void) {
    int slots[3] = {0, 1, 2};
    ui_mailbox m;
    ui_mailbox_init(&m, &slots[0], &slots[1], &slots[2]);
    CHECK(ui_mailbox_take(&m) == NULL);
    int *first = ui_mailbox_back(&m);
    *first = 10;
    CHECK(!ui_mailbox_publish(&m));
    int *second = ui_mailbox_back(&m);
    CHECK(second != first);
    *second = 20;
    CHECK(ui_mailbox_publish(&m)); // render fell behind: the pending list is replaced
    int *got = ui_mailbox_take(&m);
    CHECK(got && *got == 20);
    CHECK(ui_mailbox_take(&m) == NULL);
    // The producer never receives the slot the consumer holds.
    for (int i = 0; i < 10; i++) {
        CHECK(ui_mailbox_back(&m) != got);
        ui_mailbox_publish(&m);
    }
}

static void test_queue_bounded(void) {
    ui_queue q;
    CHECK(ui_queue_init(&q, 2, (ui_waker){0}) == UI_OK);
    ui_msg m = {.type = UI_MSG_EVENTS_DONE, .u64 = 1};
    CHECK(ui_queue_push(&q, &m) == UI_OK);
    m.u64 = 2;
    CHECK(ui_queue_push(&q, &m) == UI_OK);
    CHECK(ui_queue_push(&q, &m) == UI_ERR_FULL);
    ui_msg out;
    CHECK(ui_queue_pop(&q, &out) && out.u64 == 1);
    CHECK(ui_queue_pop(&q, &out) && out.u64 == 2);
    CHECK(!ui_queue_pop(&q, &out));
    ui_queue_destroy(&q, NULL);
}

static void test_queue_unbounded_keeps_order(void) {
    ui_queue q;
    CHECK(ui_queue_init_unbounded(&q, 4, (ui_waker){0}) == UI_OK);
    // Wrap the ring first so growth has to unwrap it.
    ui_msg m = {.type = UI_MSG_EVENTS_DONE};
    ui_msg out;
    for (uint64_t i = 0; i < 3; i++) {
        m.u64 = i;
        ui_queue_push(&q, &m);
    }
    for (int i = 0; i < 3; i++) ui_queue_pop(&q, &out);
    for (uint64_t i = 0; i < 10000; i++) {
        m.u64 = i;
        CHECK(ui_queue_push(&q, &m) == UI_OK); // never UI_ERR_FULL
    }
    CHECK_EQ_INT(ui_queue_count(&q), 10000);
    bool ordered = true;
    for (uint64_t i = 0; i < 10000; i++) ordered &= ui_queue_pop(&q, &out) && out.u64 == i;
    CHECK(ordered);
    CHECK(!ui_queue_pop(&q, &out));
    ui_queue_destroy(&q, NULL);
}

int main(void) {
    RUN(test_handle_generations);
    RUN(test_handle_claim);
    RUN(test_wire_roundtrip);
    RUN(test_writer_validation);
    RUN(test_scene_apply_and_stale);
    RUN(test_scene_rejects_malformed_atomically);
    RUN(test_spring_matches_integration);
    RUN(test_spring_frame_independent_and_retarget);
    RUN(test_clock_controls);
    RUN(test_mailbox_replaces);
    RUN(test_queue_bounded);
    RUN(test_queue_unbounded_keeps_order);
    return test_failures ? 1 : 0;
}
