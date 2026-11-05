// axe-test-luau: runs a Luau test file inside the binding's runtime host, headless, and reports to
// CTest through its exit status. What axe-test-js is for TypeScript (tests/ts/runner.c).
//
//   axe-test-luau file.luau [--scheduler single|threaded]
//
// The app has a paused clock. The file gets `axe/test`, whose side in Luau is `axe/testing`:
//   request_step(ms)     the runner applies what the turn wrote, then steps the clock by ms
//   done(failed, total)
//   hooks()              the script hooks reported so far: { { kind =, text = } }
//   c_buffer(name)       a command buffer the C writer encodes, as a `buffer`
//   c_events()           an event buffer of every event, as a `buffer`
#include <lua.h>
#include <lualib.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "axe_luau.h"
#include "script/host/host.h"
#include "ui/ui.h"
#include "wire/reference.h"

#define MAX_FRAMES 20000
#define MAX_HOOKS 256

static const char testing_source[] = {
#embed "testing.luau"
};

typedef struct runner {
    mtx_t lock;
    atomic_ullong step_ns; // requested, not yet applied
    atomic_bool done;
    int failed, total;
    struct {
        ui_trace_kind kind;
        char *text;
    } hooks[MAX_HOOKS];
    uint32_t hook_count;
} runner;

static void on_trace(void *user, const ui_trace_event *ev) {
    if (ev->kind < UI_TRACE_SCRIPT_EXCEPTION || ev->kind > UI_TRACE_SCRIPT_LEAK) return; // script events only
    runner *r = user;
    mtx_lock(&r->lock);
    if (r->hook_count < MAX_HOOKS) {
        r->hooks[r->hook_count].kind = ev->kind;
        r->hooks[r->hook_count++].text = strdup(ev->text ? ev->text : "");
    }
    mtx_unlock(&r->lock);
}

static runner *runner_of(lua_State *L) { return lua_touserdata(L, lua_upvalueindex(1)); }

static int t_request_step(lua_State *L) {
    double ms = luaL_checknumber(L, 1);
    atomic_fetch_add(&runner_of(L)->step_ns, (unsigned long long)(ms > 0 ? ms * 1e6 + 0.5 : 0));
    return 0;
}

static int t_done(lua_State *L) {
    runner *r = runner_of(L);
    r->failed = luaL_checkinteger(L, 1);
    r->total = luaL_checkinteger(L, 2);
    atomic_store(&r->done, true);
    return 0;
}

static int t_hooks(lua_State *L) {
    runner *r = runner_of(L);
    mtx_lock(&r->lock);
    lua_createtable(L, (int)r->hook_count, 0);
    for (uint32_t i = 0; i < r->hook_count; i++) {
        lua_newtable(L);
        lua_pushnumber(L, r->hooks[i].kind - UI_TRACE_SCRIPT_EXCEPTION);
        lua_setfield(L, -2, "kind");
        lua_pushstring(L, r->hooks[i].text);
        lua_setfield(L, -2, "text");
        lua_rawseti(L, -2, (int)i + 1);
    }
    mtx_unlock(&r->lock);
    return 1;
}

static int t_c_buffer(lua_State *L) {
    uint8_t *bytes;
    size_t size;
    if (!ui_test_reference_commands(luaL_checkstring(L, 1), &bytes, &size)) luaL_error(L, "c_buffer: no such case");
    memcpy(lua_newbuffer(L, size), bytes, size);
    free(bytes);
    return 1;
}

static int t_c_events(lua_State *L) {
    size_t size;
    uint8_t *bytes = ui_test_reference_events(&size);
    memcpy(lua_newbuffer(L, size), bytes, size);
    free(bytes);
    return 1;
}

static void setup(void *user, lua_State *L) {
    static const luaL_Reg functions[] = {
        {"request_step", t_request_step}, {"done", t_done},         {"hooks", t_hooks},
        {"c_buffer", t_c_buffer},         {"c_events", t_c_events},
    };
    lua_newtable(L);
    for (size_t i = 0; i < UI_ARRAY_LEN(functions); i++) {
        lua_pushlightuserdata(L, user);
        lua_pushcclosure(L, functions[i].func, functions[i].name, 1);
        lua_setfield(L, -2, functions[i].name);
    }
    axe_luau_define_value(L, "axe/test");
    axe_luau_define_source(L, "axe/testing", testing_source, sizeof testing_source);
}

int main(int argc, char **argv) {
    const char *file = NULL;
    ui_scheduler_kind scheduler = UI_SCHEDULER_FROM_ENV;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scheduler") && i + 1 < argc)
            scheduler = !strcmp(argv[++i], "single") ? UI_SCHEDULER_SINGLE : UI_SCHEDULER_THREADED;
        else file = argv[i];
    }
    if (!file) {
        fprintf(stderr, "usage: %s file.luau [--scheduler single|threaded]\n", argv[0]);
        return 2;
    }
    runner r = {0};
    mtx_init(&r.lock, mtx_plain);
    axe_luau_config config = {.setup = setup, .user = &r};
    ui_script_runtime runtime;
    axe_luau_runtime(&runtime, &config);
    ui_script_internal internal = {.runtime = &runtime};
    ui_script_desc script = {.bundle_path = file, .internal = &internal, .memory_limit = (size_t)64 << 20};
    static const char *const fonts[] = {AXE_SOURCE_DIR "/tests/fonts/NotoSans-VF.ttf"};
    ui_app_desc desc = {
        .scheduler = scheduler,
        .surface = {.kind = UI_SURFACE_HEADLESS, .width = 320, .height = 240},
        .script = &script,
        .start_paused = true,
        .text = {.font_files = fonts, .font_file_count = 1},
    };
    ui_app *app;
    if (ui_app_create(&desc, &app) != UI_OK) return 1;
    ui_hooks_subscribe(app, on_trace, &r, NULL);
    int frames = 0;
    ui_result res = UI_OK;
    while (res == UI_OK && !atomic_load(&r.done) && frames++ < MAX_FRAMES) {
        uint64_t step = atomic_exchange(&r.step_ns, 0);
        if (step) {
            // What the test wrote before stepping applies at the current time first.
            res = ui_app_frame(app);
            ui_app_clock_step(app, step);
        }
        if (res == UI_OK) res = ui_app_frame(app);
    }
    ui_app_destroy(app);
    for (uint32_t i = 0; i < r.hook_count; i++) free(r.hooks[i].text);
    if (!atomic_load(&r.done)) {
        fprintf(stderr, "%s: the tests didn't finish within %d frames\n", file, MAX_FRAMES);
        return 1;
    }
    printf("%s: %d of %d tests passed\n", file, r.total - r.failed, r.total);
    return r.failed || !r.total ? 1 : 0;
}
