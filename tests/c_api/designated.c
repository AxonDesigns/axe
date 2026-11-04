// A caller of the public C API as docs/c-api.md says to write one: every descriptor and
// config struct with designated initializers, the rest zero. CTest compiles it with
// -Werror (and the warnings that catch a struct grown under a caller) as C11, C17 and C23;
// it is never run. When a struct gains a field, this keeps compiling unchanged: that is
// the point. Add each new public struct here.
#include <stddef.h>

#include "ui/ui.h"

static ui_result on_events(void *user, const void *events, size_t size, ui_cmd_writer *cmds) {
    (void)user, (void)events, (void)size, (void)cmds;
    return UI_OK;
}

static uint64_t now(void *user) {
    (void)user;
    return 0;
}

ui_result designated(ui_cmd_writer *w, ui_handle node);

ui_result designated(ui_cmd_writer *w, ui_handle node) {
    static const char *const fonts[] = {"font.ttf"};
    static const char *const families[] = {"Noto Sans, sans-serif"};
    ui_script_desc script = {.bundle_path = "app.js", .long_task_ns = 50000000};
    ui_app_desc app = {
        .scheduler = UI_SCHEDULER_THREADED,
        .surface = {.kind = UI_SURFACE_HEADLESS, .width = 640, .height = 480, .title = "app"},
        .logic = {.on_events = on_events},
        .time = {.now_ns = now},
        .text = {.font_files = fonts, .font_file_count = 1, .system_fonts = true},
    };
    ui_app_desc scripted = {.script = &script};
    (void)app, (void)scripted;

    // The fixed pairs (docs/c-api.md) may be positional; descriptors may not.
    ui_style_decl decls[] = {{UI_STYLE_WIDTH, ui_px(100)}, {.property = UI_STYLE_OPACITY, .value = ui_number(0.5f)}};
    ui_token_decl tokens[] = {{1, ui_rgba(1, 0, 0, 1)}};
    ui_style_block_desc blocks[] = {
        {.decls = decls, .count = 2},
        {.states = UI_STATE_HOVER, .tokens = tokens, .token_count = 1},
    };
    ui_result r = ui_cmd_define_style(w, 1, blocks, 2);
    ui_text_span_desc span = {.start = 0, .end = 2, .style = {.weight = 700}, .set = UI_TEXT_SET_WEIGHT};
    ui_text_desc text = {
        .text = "hello",
        .length = 5,
        .style = {.size = 16, .weight = 400, .color = {0, 0, 0, 1}},
        .spans = &span,
        .span_count = 1,
        .families = families,
        .family_count = 1,
        .set = UI_TEXT_SET_SIZE | UI_TEXT_SET_WEIGHT,
    };
    if (r == UI_OK) r = ui_cmd_set_text(w, node, &text);
    return r;
}
