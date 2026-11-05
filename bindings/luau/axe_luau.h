// The Luau binding's runtime host (README.md): a language runtime behind the runtime host
// interface (src/script/host/runtime.h), on the logic thread, with the wire package and the
// imperative API built in. Only this directory and its tests include lua.h.
#pragma once

#include "script/host/runtime.h"

typedef struct lua_State lua_State;

// For the test runner: runs after the host's globals and modules exist, before the app.
typedef struct axe_luau_config {
    void (*setup)(void *user, lua_State *L);
    void *user;
} axe_luau_config;

// Passed to ui_script_internal.runtime. `config` may be NULL. The struct is written once per
// call and must outlive the app.
void axe_luau_runtime(ui_script_runtime *out, axe_luau_config *config);

// For `setup`: `require(name)` returns what this source evaluates to, or the value on top of the
// stack (popped).
void axe_luau_define_source(lua_State *L, const char *name, const char *source, size_t size);
void axe_luau_define_value(lua_State *L, const char *name);
