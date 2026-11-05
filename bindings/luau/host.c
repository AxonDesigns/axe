// The runtime host for Luau: what src/script/host/host.c is for JavaScript, over the Luau VM's own
// API. docs/script.md's turn, timers and errors hold, with what Luau doesn't have left out:
//
// - no microtasks, so no checkpoints and no rejections;
// - no memory the VM can borrow, so the event buffer is copied into a `buffer` once per turn, and
//   the command buffer out of one at the flush;
// - no ES modules: a bundle is one chunk, and `require` finds the modules built in here.
//
// Every call into the VM that can raise an error runs protected: outside a protected call an
// error aborts the process, and a script's failure must never stop the core.
#include "axe_luau.h"

#include <lua.h>
#include <luacode.h>
#include <lualib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unicode/utf8.h"
#include "wire/gen/wire_gen.h"

#define MODULE "luau"
#define DEFAULT_LONG_TASK_NS 50000000ull
#define KEY_LOADED "axe.loaded"
#define KEY_SOURCES "axe.sources"

static const char src_axe[] = {
#embed "lib/axe.luau"
};
static const char src_runtime[] = {
#embed "lib/wire/runtime.luau"
};
static const char src_reader[] = {
#embed "lib/wire/reader.luau"
};
static const char src_ids[] = {
#embed "lib/wire/gen/ids.luau"
};
static const char src_commands[] = {
#embed "lib/wire/gen/commands.luau"
};
static const char src_events[] = {
#embed "lib/wire/gen/events.luau"
};
static const char src_styles[] = {
#embed "lib/wire/gen/styles.luau"
};

static const struct {
    const char *name, *source;
    size_t size;
} builtin[] = {
    {"axe", src_axe, sizeof src_axe},
    {"axe/wire/runtime", src_runtime, sizeof src_runtime},
    {"axe/wire/reader", src_reader, sizeof src_reader},
    {"axe/wire/ids", src_ids, sizeof src_ids},
    {"axe/wire/commands", src_commands, sizeof src_commands},
    {"axe/wire/events", src_events, sizeof src_events},
    {"axe/wire/styles", src_styles, sizeof src_styles},
};

typedef struct luau_host {
    lua_State *L;
    ui_script_env env;
    axe_luau_config config;
    uint64_t long_task_ns;
    size_t used, limit; // the heap's bytes, and its limit (0: none)

    char *name;
    uint8_t *file;       // the bundle read from disk, if it came from a file
    const char *code;
    size_t code_size;
    bool loaded;         // the first turn has run the bundle (or failed to)
    char **args;
    uint32_t arg_count;

    uint64_t turn_ns;    // the animation clock of the current turn
    int on_error, load, dispatcher, flush; // refs; 0: none
    char *error_stack;   // the stack of the error being reported

    ui_script_timers timers;
    uint8_t *out;        // the buffer submitted this turn
    size_t out_size;
    uint64_t task_start;
} luau_host;

static luau_host *host_of(lua_State *L) { return lua_callbacks(L)->userdata; }

// ---- Reporting and protected calls -----------------------------------------------------------

// The handler of every protected call: it runs where the error was raised, so it sees the stack.
static int on_error(lua_State *L) {
    luau_host *h = host_of(L);
    luaL_tolstring(L, 1, NULL);
    luaL_traceback(L, L, NULL, 0);
    free(h->error_stack);
    h->error_stack = strdup(lua_tostring(L, -1));
    lua_pop(L, 1);
    return 1;
}

// Calls the function under its `nargs` arguments with nothing to return. An error is reported as
// uncaught, as a JS exception is, and the app goes on.
static void call(luau_host *h, int nargs, const char *context) {
    lua_State *L = h->L;
    int base = lua_gettop(L) - nargs;
    lua_getref(L, h->on_error);
    lua_insert(L, base);
    if (lua_pcall(L, nargs, 0, base) != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        ui_script_report(&h->env, UI_TRACE_SCRIPT_EXCEPTION, "Uncaught ", "", message ? message : "(no message)",
                         h->error_stack ? h->error_stack : "", context);
        free(h->error_stack);
        h->error_stack = NULL;
    }
    lua_settop(L, base - 1);
}

// A task: no checkpoint to end it, since Luau has no microtasks.
static void task_begin(luau_host *h) { h->task_start = ui_monotonic_ns(); }
static void task_end(luau_host *h, const char *what) {
    ui_script_report_long_task(&h->env, what, h->task_start, h->long_task_ns);
}

// ---- Modules -----------------------------------------------------------------------------------

// Pushes `source`, compiled, as a function.
static void load_chunk(lua_State *L, const char *name, const char *source, size_t size) {
    lua_CompileOptions options = {.optimizationLevel = 1, .debugLevel = 1};
    size_t n;
    char *bytecode = luau_compile(source, size, &options, &n);
    if (!bytecode) luaL_error(L, "out of memory compiling %s", name);
    char chunk[256];
    snprintf(chunk, sizeof chunk, "@%s", name);
    int status = luau_load(L, chunk, bytecode, n, 0);
    free(bytecode);
    if (status != 0) lua_error(L);
}

static int native_require(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, KEY_LOADED);
    lua_getfield(L, -1, name);
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, KEY_SOURCES);
    lua_getfield(L, -1, name);
    size_t size;
    const char *source = lua_tolstring(L, -1, &size);
    if (!source) luaL_error(L, "require: no module named '%s'", name);
    load_chunk(L, name, source, size);
    lua_call(L, 0, 1);
    lua_pushvalue(L, -1);
    lua_setfield(L, 2, name);
    return 1;
}

void axe_luau_define_source(lua_State *L, const char *name, const char *source, size_t size) {
    lua_getfield(L, LUA_REGISTRYINDEX, KEY_SOURCES);
    lua_pushlstring(L, source, size);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
}

void axe_luau_define_value(lua_State *L, const char *name) {
    lua_getfield(L, LUA_REGISTRYINDEX, KEY_LOADED);
    lua_insert(L, -2);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
}

// ---- Natives: `axe/host`, and print ------------------------------------------------------------

static int native_print(lua_State *L) {
    luau_host *h = host_of(L);
    luaL_Strbuf b;
    luaL_buffinit(L, &b);
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        if (i > 1) luaL_addchar(&b, ' ');
        luaL_tolstring(L, i, NULL);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    UI_INFO(h->env.log, MODULE, "%s", lua_tostring(L, -1));
    return 0;
}

static int set_hook(lua_State *L, int *slot) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    if (*slot) lua_unref(L, *slot);
    *slot = lua_ref(L, 1);
    return 0;
}

static int native_set_dispatcher(lua_State *L) { return set_hook(L, &host_of(L)->dispatcher); }
static int native_set_flush(lua_State *L) { return set_hook(L, &host_of(L)->flush); }

// submit(buffer, size): the turn's command buffer, copied out.
static int native_submit(lua_State *L) {
    luau_host *h = host_of(L);
    size_t capacity;
    void *data = luaL_checkbuffer(L, 1, &capacity);
    double size = luaL_checknumber(L, 2);
    if (h->out) luaL_error(L, "submit: one command buffer per turn");
    if (!(size >= UI_WIRE_HEADER_SIZE) || size > (double)capacity || fmod(size, 4) != 0)
        luaL_error(L, "submit: %g bytes is not a finished buffer", size);
    uint8_t *copy = malloc((size_t)size);
    if (!copy) luaL_error(L, "out of memory for commands");
    memcpy(copy, data, (size_t)size);
    h->out = copy; // the core takes this memory
    h->out_size = (size_t)size;
    return 0;
}

// set_timer(fn, ms, repeat): its id.
static int native_set_timer(lua_State *L) {
    luau_host *h = host_of(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    double ms = luaL_optnumber(L, 2, 0);
    bool repeat = lua_toboolean(L, 3);
    int ref = lua_ref(L, 1);
    uint32_t id;
    if (!ui_script_timers_add(&h->timers, h->turn_ns, ms, repeat, (void *)(intptr_t)ref, NULL, &id)) {
        lua_unref(L, ref);
        luaL_error(L, "out of memory for a timer");
    }
    lua_pushnumber(L, id);
    return 1;
}

static int native_clear_timer(lua_State *L) {
    luau_host *h = host_of(L);
    ui_script_timer t;
    if (ui_script_timers_clear(&h->timers, luaL_checknumber(L, 1), &t)) lua_unref(L, (int)(intptr_t)t.fn);
    return 0;
}

static int native_animation_time(lua_State *L) {
    lua_pushnumber(L, (double)host_of(L)->turn_ns);
    return 1;
}

// ---- Lifecycle ---------------------------------------------------------------------------------

static void *allocate(void *user, void *ptr, size_t old_size, size_t size) {
    luau_host *h = user;
    if (!size) {
        free(ptr);
        h->used -= old_size;
        return NULL;
    }
    if (h->limit && h->used - old_size + size > h->limit) return NULL;
    void *p = realloc(ptr, size);
    if (p) h->used += size - old_size;
    return p;
}

static void on_panic(lua_State *L, int status) {
    UI_ERROR(host_of(L)->env.log, MODULE, "an error outside a protected call (status %d)", status);
}

static int load_app(lua_State *L) {
    luau_host *h = host_of(L);
    load_chunk(L, h->name, h->code, h->code_size);
    lua_call(L, 0, 0);
    return 0;
}

// The VM's globals and the modules; protected, as any of it can run out of memory.
static int install(lua_State *L) {
    luau_host *h = lua_touserdata(L, 1);
    luaL_openlibs(L);
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, KEY_LOADED);
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, KEY_SOURCES);
    lua_pushcfunction(L, native_print, "print");
    lua_setglobal(L, "print");
    lua_pushcfunction(L, native_require, "require");
    lua_setglobal(L, "require");
    lua_pushcfunction(L, on_error, "on_error");
    h->on_error = lua_ref(L, -1);
    lua_pop(L, 1);
    lua_pushcfunction(L, load_app, "load_app");
    h->load = lua_ref(L, -1);
    lua_pop(L, 1);

    for (size_t i = 0; i < UI_ARRAY_LEN(builtin); i++)
        axe_luau_define_source(L, builtin[i].name, builtin[i].source, builtin[i].size);

    static const luaL_Reg natives[] = {
        {"set_dispatcher", native_set_dispatcher}, {"set_flush", native_set_flush},
        {"submit", native_submit},                 {"set_timer", native_set_timer},
        {"clear_timer", native_clear_timer},       {"animation_time", native_animation_time},
    };
    lua_newtable(L);
    for (size_t i = 0; i < UI_ARRAY_LEN(natives); i++) {
        lua_pushcfunction(L, natives[i].func, natives[i].name);
        lua_setfield(L, -2, natives[i].name);
    }
    lua_createtable(L, (int)h->arg_count, 0);
    for (uint32_t i = 0; i < h->arg_count; i++) {
        lua_pushstring(L, h->args[i]);
        lua_rawseti(L, -2, (int)i + 1);
    }
    lua_setfield(L, -2, "args");
    axe_luau_define_value(L, "axe/host");
    if (h->config.setup) h->config.setup(h->config.user, L);
    return 0;
}

static void luau_destroy(void *self) {
    luau_host *h = self;
    if (!h) return;
    if (h->L) lua_close(h->L);
    ui_script_timers_free(&h->timers);
    for (uint32_t i = 0; i < h->arg_count; i++) free(h->args[i]);
    free(h->args);
    free(h->error_stack);
    free(h->out);
    free(h->file);
    free(h->name);
    free(h);
}

static ui_result luau_create(const ui_script_desc *desc, const ui_script_env *env, void *config, void **out) {
    luau_host *h = calloc(1, sizeof *h);
    if (!h) return UI_ERR_OUT_OF_MEMORY;
    h->env = *env;
    if (config) h->config = *(axe_luau_config *)config;
    h->long_task_ns = desc->long_task_ns ? desc->long_task_ns : DEFAULT_LONG_TASK_NS;
    h->limit = desc->memory_limit;
    ui_script_timers_init(&h->timers);

    const char *name = desc->bundle_name ? desc->bundle_name : desc->bundle_path ? desc->bundle_path : "app.luau";
    ui_result r = (h->name = strdup(name)) ? UI_OK : UI_ERR_OUT_OF_MEMORY;
    if (r == UI_OK && desc->arg_count) {
        h->args = calloc(desc->arg_count, sizeof *h->args);
        r = h->args ? UI_OK : UI_ERR_OUT_OF_MEMORY;
        for (uint32_t i = 0; r == UI_OK && i < desc->arg_count; i++, h->arg_count++)
            if (!(h->args[i] = strdup(desc->args[i] ? desc->args[i] : ""))) r = UI_ERR_OUT_OF_MEMORY;
    }
    h->code = desc->bundle;
    h->code_size = desc->bundle_size;
    if (r == UI_OK && desc->bundle_path) {
        h->code = (const char *)(h->file = ui_script_read_file(desc->bundle_path, &h->code_size));
        if (!h->file) {
            UI_ERROR(env->log, MODULE, "cannot read the bundle %s", desc->bundle_path);
            r = UI_ERR_IO;
        }
    }
    if (r == UI_OK && !h->code) r = UI_ERR_INVALID_ARGUMENT;
    if (r == UI_OK && !(h->L = lua_newstate(allocate, h))) r = UI_ERR_OUT_OF_MEMORY;
    if (r == UI_OK) {
        lua_callbacks(h->L)->userdata = h;
        lua_callbacks(h->L)->panic = on_panic;
        if (lua_cpcall(h->L, install, h) != LUA_OK) {
            UI_ERROR(env->log, MODULE, "installing the host API: %s", lua_tostring(h->L, -1));
            r = UI_ERR_PLATFORM;
        }
    }
    if (r != UI_OK) {
        luau_destroy(h);
        return r;
    }
    *out = h;
    return UI_OK;
}

// ---- The turn ------------------------------------------------------------------------------

static void run_timers(luau_host *h) {
    lua_State *L = h->L;
    uint64_t batch = h->timers.next_seq;
    ui_script_timer t;
    while (ui_script_timers_pop(&h->timers, h->turn_ns, batch, h->env.log, &t)) {
        int ref = (int)(intptr_t)t.fn;
        lua_getref(L, ref);
        task_begin(h);
        call(h, 0, t.interval ? "in an interval" : "in a timeout");
        task_end(h, "a timer");
        if (!t.interval) lua_unref(L, ref);
    }
}

typedef struct event_copy {
    const uint8_t *bytes;
    size_t size;
    int ref;
} event_copy;

static int copy_events(lua_State *L) {
    event_copy *e = lua_touserdata(L, 1);
    memcpy(lua_newbuffer(L, e->size), e->bytes, e->size);
    e->ref = lua_ref(L, -1);
    return 0;
}

static const char *event_name(uint16_t type) {
    switch (type) {
    case UI_EVENT_FRAME_COMMITTED: return "event frame_committed";
    case UI_EVENT_SURFACE_RESIZED: return "event surface_resized";
    case UI_EVENT_SURFACE_CLOSE_REQUESTED: return "event surface_close_requested";
    case UI_EVENT_TIMER_FIRED: return "event timer_fired";
    case UI_EVENT_FONT_LOADED: return "event font_loaded";
    case UI_EVENT_LAYOUT_MEASURED: return "event layout_measured";
    case UI_EVENT_CLOCK: return "the turn's start";
    default: return "an event";
    }
}

static void luau_turn(void *self, const uint8_t *events, size_t size, uint8_t **cmds, size_t *cmds_size) {
    luau_host *h = self;
    lua_State *L = h->L;
    *cmds = NULL;
    *cmds_size = 0;
    ui_wire_reader rd;
    if (ui_wire_reader_init(&rd, UI_WIRE_MAGIC_EVENT, events, size) != UI_OK) {
        UI_ERROR(h->env.log, MODULE, "malformed event buffer (%zu bytes)", size);
        return;
    }
    ui_script_turn_time(rd, &h->turn_ns);
    if (!h->loaded) {
        h->loaded = true;
        lua_getref(L, h->load);
        task_begin(h);
        call(h, 0, "loading the app");
        task_end(h, "evaluating the app");
    }

    event_copy copy = {events, size, 0};
    if (lua_cpcall(L, copy_events, &copy) != LUA_OK) {
        UI_ERROR(h->env.log, MODULE, "out of memory for the turn's events");
        lua_pop(L, 1);
    }
    int top = lua_gettop(L);
    if (copy.ref) lua_getref(L, copy.ref);
    ui_wire_op op;
    ui_result res;
    while ((res = ui_wire_reader_next(&rd, &op)) == UI_OK) {
        if (!ui_wire_event_framed(op.opcode, op.payload, op.payload_size)) {
            res = UI_ERR_MALFORMED;
            break;
        }
        if (ui_script_timers_fired(&h->timers, &op)) {
            run_timers(h);
            continue;
        }
        if (!copy.ref || !h->dispatcher) continue;
        const char *what = event_name(op.opcode);
        lua_getref(L, h->dispatcher);
        lua_pushvalue(L, top + 1);
        lua_pushnumber(L, op.opcode);
        lua_pushnumber(L, (double)(op.payload - events));
        task_begin(h);
        call(h, 3, what);
        task_end(h, what);
    }
    if (res != UI_ERR_END) UI_ERROR(h->env.log, MODULE, "malformed event buffer: the rest of it is skipped");
    lua_settop(L, top);
    if (copy.ref) lua_unref(L, copy.ref);

    // One command buffer per turn: what Luau wrote, then the wire timer for the next timer.
    if (h->flush) {
        lua_getref(L, h->flush);
        task_begin(h);
        call(h, 0, "flushing commands");
        task_end(h, "flushing commands");
    }
    ui_script_timers_arm(&h->timers, h->env.log, &h->out, &h->out_size);
    *cmds = h->out;
    *cmds_size = h->out_size;
    h->out = NULL;
    h->out_size = 0;
}

void axe_luau_runtime(ui_script_runtime *out, axe_luau_config *config) {
    *out = (ui_script_runtime){.create = luau_create, .turn = luau_turn, .destroy = luau_destroy, .config = config};
}
