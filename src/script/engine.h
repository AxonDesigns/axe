// The JS engine interface: everything the runtime host (src/script/host/) needs from an
// engine, and nothing that names one. QuickJS-ng implements it in src/script/quickjs/, the
// only place that includes quickjs.h. docs/script.md, "Engine interface", is the contract a
// future engine (JavaScriptCore, V8) or language (Dart, Luau) implements.
//
// Conventions:
// - An engine (ui_js) is one heap and one realm, used by one thread at a time: the thread
//   that created it, or the last one that called ui_js_attach_thread.
// - Values (ui_js_val) are handles local to a scope. They stay valid until the scope that was
//   innermost when they were created closes. ui_js_ref keeps a value across scopes.
// - A function returning ui_js_val returns the empty value (ui_js_is_empty) when it leaves an
//   exception pending; a function returning bool returns false. Nothing else throws. Only one
//   exception is pending at a time, until ui_js_take_exception or until JS catches it.
// - Strings cross the interface as UTF-8.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui/result.h"

typedef struct ui_js ui_js;

// A value in the current scope. id 0 is the empty value (no value: an exception is pending).
typedef struct ui_js_val {
    uint32_t id;
} ui_js_val;

static inline bool ui_js_is_empty(ui_js_val v) { return v.id == 0; }

// A reference that outlives scopes: strong (keeps the value alive) or weak (an object only;
// empty once the object is collected).
typedef struct ui_js_ref ui_js_ref;

typedef enum ui_js_type {
    UI_JS_UNDEFINED,
    UI_JS_NULL,
    UI_JS_BOOL,
    UI_JS_NUMBER,
    UI_JS_BIGINT,
    UI_JS_STRING,
    UI_JS_SYMBOL,
    UI_JS_OBJECT,
    UI_JS_FUNCTION,
} ui_js_type;

typedef enum ui_js_error_kind {
    UI_JS_ERROR,
    UI_JS_TYPE_ERROR,
    UI_JS_RANGE_ERROR,
    UI_JS_SYNTAX_ERROR,
    UI_JS_INTERNAL_ERROR,
} ui_js_error_kind;

typedef enum ui_js_promise_state {
    UI_JS_PENDING,
    UI_JS_FULFILLED,
    UI_JS_REJECTED,
    UI_JS_NOT_A_PROMISE,
} ui_js_promise_state;

// A module's code, as the host's loader provides it. `bytes` stays owned by the host and
// only has to live through the load call.
typedef struct ui_js_module_source {
    const void *bytes;
    size_t size;
    // Engine bytecode from ui_js_compile_module instead of source. Bytecode is trusted input:
    // an engine may not validate it, so it must come from the host's own build.
    bool bytecode;
} ui_js_module_source;

typedef struct ui_js_desc {
    size_t memory_limit; // bytes the heap may use; 0: no limit
    size_t stack_limit;  // native stack bytes JS may use; 0: the engine's default
    void *user;          // passed to the callbacks below
    // Module loading, for `import` in a module. resolve returns the module's name
    // (malloc'd; NULL: not found), given the name of the importing module. load fills *out
    // and returns true, or returns false (the import throws). Optional: without them, only
    // the modules given to ui_js_eval_module and ui_js_define_module exist.
    char *(*resolve)(void *user, const char *base, const char *specifier);
    bool (*load)(void *user, const char *name, ui_js_module_source *out);
    // A promise was rejected with no handler (`handled` false), or a handler was added to one
    // reported before (true). Called during microtasks; must not run JS.
    void (*rejection)(void *user, ui_js *js, ui_js_val promise, ui_js_val reason, bool handled);
} ui_js_desc;

// Thread: becomes the engine's thread. Ownership: destroy with ui_js_destroy.
ui_result ui_js_create(const ui_js_desc *desc, ui_js **out);
// Frees everything, running the finalizers of native objects. Refs must be freed first.
void ui_js_destroy(ui_js *js);
// Makes the calling thread the engine's thread (its stack is the one checked for overflow).
void ui_js_attach_thread(ui_js *js);

// "quickjs-ng" and its version: what bytecode depends on.
const char *ui_js_engine_name(void);
const char *ui_js_engine_version(void);

// ---- Scopes ------------------------------------------------------------------------------

// Opens a scope and returns its mark. Values created from here on belong to it.
uint32_t ui_js_scope_open(ui_js *js);
// Releases every value created since `mark`, which must be the innermost open scope's.
void ui_js_scope_close(ui_js *js, uint32_t mark);
// Closes the scope but keeps `v` alive, as a value of the scope around it.
ui_js_val ui_js_scope_escape(ui_js *js, uint32_t mark, ui_js_val v);

// ---- Values --------------------------------------------------------------------------------

ui_js_val ui_js_undefined(ui_js *js);
ui_js_val ui_js_null(ui_js *js);
ui_js_val ui_js_bool(ui_js *js, bool b);
ui_js_val ui_js_number(ui_js *js, double n);
ui_js_val ui_js_string(ui_js *js, const char *utf8, size_t len); // invalid UTF-8: U+FFFD
ui_js_val ui_js_object(ui_js *js);
ui_js_val ui_js_array(ui_js *js);
ui_js_val ui_js_global(ui_js *js);
// An Error of `kind` with this message (not thrown).
ui_js_val ui_js_error(ui_js *js, ui_js_error_kind kind, const char *message);

ui_js_type ui_js_typeof(ui_js *js, ui_js_val v);
bool ui_js_is_array(ui_js *js, ui_js_val v);
bool ui_js_is_error(ui_js *js, ui_js_val v);
bool ui_js_strict_equal(ui_js *js, ui_js_val a, ui_js_val b);
// JS truthiness; never runs JS.
bool ui_js_truthy(ui_js *js, ui_js_val v);
// ToNumber (may run JS: valueOf).
bool ui_js_to_number(ui_js *js, ui_js_val v, double *out);
// ToString as UTF-8, NUL-terminated (may run JS). Ownership: free() it. NULL: exception.
char *ui_js_to_utf8(ui_js *js, ui_js_val v, size_t *len);

// Properties (getters and setters may run JS).
bool ui_js_get(ui_js *js, ui_js_val obj, const char *key, ui_js_val *out);
bool ui_js_set(ui_js *js, ui_js_val obj, const char *key, ui_js_val v);
bool ui_js_get_index(ui_js *js, ui_js_val obj, uint32_t i, ui_js_val *out);
bool ui_js_set_index(ui_js *js, ui_js_val obj, uint32_t i, ui_js_val v);
// Defines an own property: writable and configurable, enumerable when `enumerable`.
bool ui_js_define(ui_js *js, ui_js_val obj, const char *key, ui_js_val v, bool enumerable);
// The array's length, or of an array-like's `length` (0 if absent).
bool ui_js_length(ui_js *js, ui_js_val obj, uint32_t *out);

bool ui_js_call(ui_js *js, ui_js_val fn, ui_js_val this_val, uint32_t argc, const ui_js_val *argv, ui_js_val *out);
bool ui_js_construct(ui_js *js, ui_js_val ctor, uint32_t argc, const ui_js_val *argv, ui_js_val *out);

// ---- Native functions and objects ---------------------------------------------------------

typedef struct ui_js_call_info {
    ui_js_val this_val;
    const ui_js_val *args; // argc values; missing arguments are not padded with undefined
    uint32_t argc;
    void *data;            // as given to ui_js_function
} ui_js_call_info;

// Returns the result, or the empty value after ui_js_throw (the call then throws). Values
// it creates are released when it returns, except the one it returns.
typedef ui_js_val (*ui_js_native)(ui_js *js, const ui_js_call_info *call);

ui_js_val ui_js_function(ui_js *js, const char *name, uint32_t length, ui_js_native fn, void *data);
// An object carrying `data`, finalized (finalize(data)) when collected or at ui_js_destroy.
ui_js_val ui_js_native_object(ui_js *js, void *data, void (*finalize)(void *data));
// The data of a native object; NULL for any other value.
void *ui_js_native_data(ui_js *js, ui_js_val obj);

// ---- Exceptions ---------------------------------------------------------------------------

// Throws `v` and returns the empty value, for a native function to return.
ui_js_val ui_js_throw(ui_js *js, ui_js_val v);
[[gnu::format(printf, 3, 4)]] ui_js_val ui_js_throw_error(ui_js *js, ui_js_error_kind kind, const char *fmt, ...);
bool ui_js_has_exception(ui_js *js);

// What a thrown value says, in UTF-8. Ownership: ui_js_exception_free.
typedef struct ui_js_exception {
    char *name;        // "TypeError"; "" for a value that isn't an Error
    char *message;     // the message, or the value as a string
    char *stack;       // the stack trace, one frame per line; "" if none
    bool uncatchable;  // an interrupt or out-of-memory error, which JS can't catch
} ui_js_exception;

// Takes the pending exception (none is pending afterwards): its description, and the value
// itself in *value when not NULL. False if none was pending.
bool ui_js_take_exception(ui_js *js, ui_js_exception *out, ui_js_val *value);
// Describes any value as an exception (a rejection's reason).
void ui_js_describe(ui_js *js, ui_js_val v, ui_js_exception *out);
void ui_js_exception_free(ui_js_exception *e);

// ---- References ---------------------------------------------------------------------------

// NULL when out of memory, or for a weak reference to something that isn't an object.
ui_js_ref *ui_js_ref_new(ui_js *js, ui_js_val v, bool weak);
// The referenced value in the current scope; false (and *out empty) if a weak reference's
// object was collected.
bool ui_js_ref_get(ui_js *js, const ui_js_ref *r, ui_js_val *out);
void ui_js_ref_free(ui_js *js, ui_js_ref *r);

// ---- Modules and scripts ------------------------------------------------------------------

// Exposes `exports`' own enumerable properties, as they are now, as module `name`'s named
// exports (the host's `axe:host`). Imports resolve to it before the loader is asked.
bool ui_js_define_module(ui_js *js, const char *name, ui_js_val exports);
// Compiles and evaluates an ES module named `name`, loading its imports. *result is the
// evaluation's promise (top-level await): rejected if evaluation throws. False for a
// syntax error or a failed import.
bool ui_js_eval_module(ui_js *js, const char *name, const ui_js_module_source *src, ui_js_val *result);
// Compiles a module (without evaluating it) to bytecode. Ownership: free() *out.
bool ui_js_compile_module(ui_js *js, const char *name, const char *src, size_t len, uint8_t **out,
                          size_t *size);
// Evaluates a classic script; *result is its completion value.
bool ui_js_eval_script(ui_js *js, const char *name, const char *src, size_t len, ui_js_val *result);

// ---- Microtasks and promises --------------------------------------------------------------

// Runs queued jobs (promise reactions, queued microtasks) until none is left, including those
// they queue. A job that throws is passed to `on_error` (which takes the exception) and the
// others still run. Returns how many ran.
uint32_t ui_js_run_microtasks(ui_js *js, void (*on_error)(void *user, ui_js *js), void *user);
bool ui_js_has_microtasks(ui_js *js);
// Queues fn() as a microtask.
bool ui_js_enqueue_microtask(ui_js *js, ui_js_val fn);
// A new pending promise and its resolving functions.
ui_js_val ui_js_promise(ui_js *js, ui_js_val *resolve, ui_js_val *reject);
// The state of a promise, and its value or reason in *result when settled.
ui_js_promise_state ui_js_promise_get(ui_js *js, ui_js_val promise, ui_js_val *result);

// ---- Binary data --------------------------------------------------------------------------

// An ArrayBuffer over memory the host owns: no copy. free_fn(user, data) runs when the
// buffer is collected or detached (it may be NULL).
ui_js_val ui_js_array_buffer_external(ui_js *js, void *data, size_t size,
                                      void (*free_fn)(void *user, void *data), void *user);
ui_js_val ui_js_array_buffer_copy(ui_js *js, const void *data, size_t size);
// The bytes of an attached ArrayBuffer (no copy); false for anything else.
bool ui_js_array_buffer_data(ui_js *js, ui_js_val v, void **data, size_t *size);
// The bytes a typed array or DataView views (no copy); false for anything else.
bool ui_js_view_data(ui_js *js, ui_js_val v, void **data, size_t *size);
// Detaches an ArrayBuffer: JS sees it empty from now on, and its free_fn runs.
void ui_js_array_buffer_detach(ui_js *js, ui_js_val v);

// ---- Limits and diagnostics ---------------------------------------------------------------

// Polled while JS runs; returning true interrupts it with an uncatchable error.
void ui_js_set_interrupt(ui_js *js, bool (*poll)(void *user), void *user);
void ui_js_set_memory_limit(ui_js *js, size_t bytes);

typedef struct ui_js_memory {
    uint64_t used_bytes;      // allocated by the heap
    uint64_t limit_bytes;     // 0: no limit
    uint64_t allocations;
    uint64_t objects;
    uint64_t strings, string_bytes;
    uint64_t functions, bytecode_bytes;
    uint64_t array_buffer_bytes;
} ui_js_memory;

void ui_js_memory_stats(ui_js *js, ui_js_memory *out);
// Collects garbage now (cycles included); FinalizationRegistry callbacks are queued as
// microtasks.
void ui_js_gc(ui_js *js);
