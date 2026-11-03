// Glyph uploads survive display-list replacement (docs/text.md, "Atlas uploads").
//
// The core produces frames whose display lists introduce new glyphs while the render actor
// does not draw them, so the mailbox replaces those lists. A later frame shows text whose
// glyphs were introduced by a replaced list: they are cached, so no new upload is sent,
// and the frame must still look exactly like the same text rendered by a fresh app.
//   - single scheduler: the core steps alone (ui_app_step_core), render runs later;
//   - threaded scheduler: a hook holds the render thread inside encode while the core
//     publishes 20 frames.
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "runtime/app_internal.h"
#include "test.h"
#include "ui/ui.h"

#define WIDTH 320u
#define HEIGHT 120u
#define FONT(name) UI_TEST_FONTS "/" name

static const char *const font_files[] = {FONT("NotoSans-VF.ttf")};
static const char TEXT[] = "Cached glyphs from a replaced frame";

typedef struct client {
    ui_app *app;
    ui_handle_allocator *handles;
    ui_cmd_writer *w;
    ui_handle node;
} client;

// Sends a command buffer through the logic -> core channel, as the logic actor would.
static bool send(client *c) {
    uint8_t *bytes;
    size_t size;
    if (ui_cmd_writer_take(c->w, &bytes, &size) != UI_OK) return false;
    ui_msg msg = {.type = UI_MSG_CMD_BUFFER, .ptr = bytes, .size = size};
    if (ui_queue_push(&c->app->core.from_logic, &msg) == UI_OK) return true;
    free(bytes);
    return false;
}

static bool set_text(client *c, float size) {
    static const char *const families[] = {"Noto Sans"};
    ui_text_desc d = {.set = UI_TEXT_SET_ALL,
        .text = TEXT, .length = sizeof TEXT - 1,
        .style = {.family = 0, .weight = 400, .size = size, .color = {0, 0, 0, 1}},
        .families = families, .family_count = 1,
    };
    return ui_cmd_set_text(c->w, c->node, &d) == UI_OK && send(c);
}

static bool open_app(client *c, ui_scheduler_kind scheduler, float size) {
    memset(c, 0, sizeof *c);
    ui_app_desc desc = {
        .scheduler = scheduler,
        .gpu = UI_GPU_SOFTWARE,
        .surface = {.kind = UI_SURFACE_HEADLESS, .width = WIDTH, .height = HEIGHT},
        .start_paused = true,
        .text = {.font_files = font_files, .font_file_count = 1},
    };
    if (ui_app_create(&desc, &c->app) != UI_OK || ui_handle_allocator_create(&c->handles) != UI_OK ||
        ui_cmd_writer_create(&c->w) != UI_OK || ui_handle_alloc(c->handles, &c->node) != UI_OK)
        return false;
    ui_cmd_create_node(c->w, c->node, ui_app_main_surface(c->app));
    ui_cmd_set_rect(c->w, c->node, 0, 0, WIDTH, HEIGHT);
    ui_cmd_set_color(c->w, c->node, 1, 1, 1, 1);
    return set_text(c, size) && ui_app_frame(c->app) == UI_OK;
}

static void close_app(client *c) {
    if (c->app) ui_app_destroy(c->app);
    ui_cmd_writer_destroy(c->w);
    ui_handle_allocator_destroy(c->handles);
}

static bool capture(client *c, ui_image *img) {
    return ui_surface_capture(c->app, ui_app_main_surface(c->app), img) == UI_OK;
}

// The text at `size` in a fresh app: the image the cached glyphs must reproduce.
static bool reference(float size, ui_image *img) {
    client c;
    bool ok = open_app(&c, UI_SCHEDULER_SINGLE, size) && capture(&c, img);
    close_app(&c);
    return ok;
}

static void compare(const ui_image *got, const ui_image *want) {
    CHECK(got->width == want->width && got->height == want->height);
    CHECK(!memcmp(got->pixels, want->pixels, (size_t)want->stride * want->height));
}

static void test_single(void) {
    ui_image want = {0}, got = {0};
    CHECK(reference(19, &want));
    client c;
    CHECK(open_app(&c, UI_SCHEDULER_SINGLE, 12)); // something else is drawn first
    ui_app_stats before, after;
    ui_app_get_stats(c.app, &before);
    // Frame X introduces the 19 px glyphs; frame Y replaces it before render runs.
    CHECK(set_text(&c, 19) && ui_app_step_core(c.app) == UI_OK);
    CHECK(set_text(&c, 23) && ui_app_step_core(c.app) == UI_OK);
    CHECK(ui_app_frame(c.app) == UI_OK); // render draws Y, applying X's and Y's uploads
    ui_app_get_stats(c.app, &after);
    CHECK_EQ_INT(after.display_lists_replaced, before.display_lists_replaced + 1);
    CHECK_EQ_INT(after.atlas_updates, before.atlas_updates + 2);
    // Back to 19 px: every glyph is cached, nothing is uploaded, and the picture is right.
    CHECK(set_text(&c, 19) && ui_app_frame(c.app) == UI_OK);
    ui_app_stats last;
    ui_app_get_stats(c.app, &last);
    CHECK_EQ_INT(last.atlas_updates, after.atlas_updates);
    CHECK_EQ_INT(last.glyphs_rasterized, after.glyphs_rasterized);
    CHECK(capture(&c, &got));
    compare(&got, &want);
    close_app(&c);
    ui_image_free(&want);
    ui_image_free(&got);
}

// Holds the render thread at the start of encode while `hold` is set.
typedef struct render_gate {
    mtx_t lock;
    cnd_t cond;
    bool hold;
    _Atomic bool held; // render is waiting at the gate
} render_gate;

static void gate_hook(void *user, const ui_trace_event *ev) {
    render_gate *g = user;
    if (ev->kind != UI_TRACE_PHASE_BEGIN || ev->phase != UI_PHASE_ENCODE) return;
    mtx_lock(&g->lock);
    while (g->hold) {
        atomic_store(&g->held, true);
        cnd_wait(&g->cond, &g->lock);
    }
    atomic_store(&g->held, false);
    mtx_unlock(&g->lock);
}

static void test_threaded(void) {
    ui_image want = {0}, got = {0};
    CHECK(reference(17, &want));
    client c;
    CHECK(open_app(&c, UI_SCHEDULER_THREADED, 12));
    render_gate gate = {.hold = true};
    mtx_init(&gate.lock, mtx_plain);
    cnd_init(&gate.cond);
    atomic_init(&gate.held, false);
    uint32_t hook;
    ui_result hr = ui_hooks_subscribe(c.app, gate_hook, &gate, &hook);
    if (hr == UI_ERR_UNSUPPORTED) {
        fprintf(stderr, "  skipped: built without hooks\n");
        close_app(&c);
        ui_image_free(&want);
        return;
    }
    CHECK(hr == UI_OK);
    ui_app_stats before, after;
    ui_app_get_stats(c.app, &before);
    // Frame 1 reaches render, which stops in encode; frames 2..20 (new glyphs each, at
    // 13..31 px) replace one another in the mailbox while their uploads queue up.
    for (int i = 0; i < 20; i++) {
        CHECK(set_text(&c, 13.f + (float)i) && ui_app_step_core(c.app) == UI_OK);
        if (i == 0) {
            while (!atomic_load(&gate.held)) thrd_yield();
        }
    }
    // 17 px was introduced by frame 5, which was replaced and never drawn.
    CHECK(set_text(&c, 17) && ui_app_step_core(c.app) == UI_OK);
    ui_app_get_stats(c.app, &after);
    CHECK(after.display_lists_replaced >= before.display_lists_replaced + 19);
    CHECK_EQ_INT(after.atlas_updates, before.atlas_updates + 20); // nothing new for 17 px
    mtx_lock(&gate.lock);
    gate.hold = false;
    cnd_broadcast(&gate.cond);
    mtx_unlock(&gate.lock);
    CHECK(ui_app_frame(c.app) == UI_OK); // render finishes frame 1, then draws the last list
    ui_hooks_unsubscribe(c.app, hook);
    CHECK(capture(&c, &got));
    compare(&got, &want);
    close_app(&c);
    cnd_destroy(&gate.cond);
    mtx_destroy(&gate.lock);
    ui_image_free(&want);
    ui_image_free(&got);
}

int main(void) {
    RUN(test_single);
    RUN(test_threaded);
    return test_failures != 0;
}
