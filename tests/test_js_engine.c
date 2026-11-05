// The engine interface (src/script/engine.h) on QuickJS-ng: values and scopes, native
// functions and objects, exceptions with stack traces, strong and weak references, modules
// from source and bytecode, microtasks and rejections, external ArrayBuffers, the interrupt
// hook, memory limits and stats.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "script/engine.h"
#include "test.h"

static ui_js *make(const ui_js_desc *d) {
    ui_js *js = NULL;
    CHECK(ui_js_create(d ? d : &(ui_js_desc){0}, &js) == UI_OK);
    return js;
}

// Evaluates a script and returns its value (empty on exception).
static ui_js_val eval(ui_js *js, const char *src) {
    ui_js_val v;
    ui_js_eval_script(js, "<test>", src, strlen(src), &v);
    return v;
}

static bool equals(ui_js *js, ui_js_val v, const char *want) {
    char *s = ui_js_to_utf8(js, v, NULL);
    bool ok = s && !strcmp(s, want);
    if (!ok) fprintf(stderr, "  got \"%s\", want \"%s\"\n", s ? s : "(exception)", want);
    free(s);
    return ok;
}

static double number(ui_js *js, ui_js_val v) {
    double d = -1;
    ui_js_to_number(js, v, &d);
    return d;
}

static void test_values(void) {
    ui_js *js = make(NULL);
    uint32_t scope = ui_js_scope_open(js);
    CHECK(ui_js_typeof(js, ui_js_undefined(js)) == UI_JS_UNDEFINED);
    CHECK(ui_js_typeof(js, ui_js_null(js)) == UI_JS_NULL);
    CHECK(ui_js_typeof(js, ui_js_bool(js, true)) == UI_JS_BOOL);
    CHECK(ui_js_typeof(js, ui_js_number(js, 2.5)) == UI_JS_NUMBER);
    CHECK(ui_js_typeof(js, eval(js, "10n")) == UI_JS_BIGINT);
    CHECK(ui_js_typeof(js, eval(js, "Symbol()")) == UI_JS_SYMBOL);
    CHECK(ui_js_typeof(js, eval(js, "() => 1")) == UI_JS_FUNCTION);
    CHECK(ui_js_typeof(js, ui_js_object(js)) == UI_JS_OBJECT);
    CHECK(ui_js_is_array(js, ui_js_array(js)));
    CHECK(ui_js_is_error(js, ui_js_error(js, UI_JS_RANGE_ERROR, "no")));
    CHECK(!ui_js_truthy(js, eval(js, "''")) && ui_js_truthy(js, eval(js, "[]")));

    // UTF-8 both ways, including a character outside the BMP.
    const char *text = "h\xC3\xA9llo \xF0\x9F\x98\x80";
    ui_js_val s = ui_js_string(js, text, strlen(text));
    CHECK(equals(js, s, text));
    ui_js_val len;
    CHECK(ui_js_get(js, s, "length", &len) && number(js, len) == 8); // UTF-16 code units

    // Properties, indexes, defined properties.
    ui_js_val o = ui_js_object(js), got;
    CHECK(ui_js_set(js, o, "a", ui_js_number(js, 1)));
    CHECK(ui_js_define(js, o, "hidden", ui_js_number(js, 2), false));
    CHECK(ui_js_get(js, o, "a", &got) && number(js, got) == 1);
    ui_js_val global = ui_js_global(js);
    CHECK(ui_js_set(js, global, "o", o));
    CHECK(equals(js, eval(js, "Object.keys(o).join()"), "a"));
    ui_js_val arr = ui_js_array(js);
    CHECK(ui_js_set_index(js, arr, 2, ui_js_number(js, 7)));
    uint32_t n;
    CHECK(ui_js_length(js, arr, &n) && n == 3);
    CHECK(ui_js_get_index(js, arr, 2, &got) && number(js, got) == 7);
    CHECK(ui_js_strict_equal(js, got, ui_js_number(js, 7)));

    // Calls and constructors.
    ui_js_val add = eval(js, "(function (a, b) { return a + b + this.k; })"), self = eval(js, "({k: 100})");
    ui_js_val args[2] = {ui_js_number(js, 1), ui_js_number(js, 2)};
    CHECK(ui_js_call(js, add, self, 2, args, &got) && number(js, got) == 103);
    ui_js_val date;
    CHECK(ui_js_get(js, global, "Date", &date));
    CHECK(ui_js_construct(js, date, 1, (ui_js_val[]){ui_js_number(js, 0)}, &got) && ui_js_typeof(js, got) == UI_JS_OBJECT);
    ui_js_scope_close(js, scope);
    ui_js_destroy(js);
}

static void test_scopes(void) {
    ui_js *js = make(NULL);
    uint32_t outer = ui_js_scope_open(js);
    uint32_t inner = ui_js_scope_open(js);
    ui_js_val kept = ui_js_scope_escape(js, inner, eval(js, "'kept'"));
    CHECK(equals(js, kept, "kept"));
    // Many values in one scope grow the stack; closing it frees them all.
    for (int i = 0; i < 10000; i++) ui_js_number(js, i);
    ui_js_scope_close(js, outer);
    CHECK(ui_js_scope_open(js) == outer);
    ui_js_destroy(js);
}

static int finalized;
static void finalize(void *data) { finalized += *(int *)data; }

static ui_js_val native_sum(ui_js *js, const ui_js_call_info *call) {
    double sum = *(double *)call->data;
    for (uint32_t i = 0; i < call->argc; i++) {
        double d;
        if (!ui_js_to_number(js, call->args[i], &d)) return (ui_js_val){0};
        sum += d;
    }
    return ui_js_number(js, sum);
}

static ui_js_val native_throws(ui_js *js, const ui_js_call_info *call) {
    (void)call;
    return ui_js_throw_error(js, UI_JS_TYPE_ERROR, "bad %s", "argument");
}

static ui_js_val native_forgets(ui_js *js, const ui_js_call_info *call) {
    (void)js, (void)call;
    return (ui_js_val){0}; // no value, no exception: the adapter throws for it
}

static void test_natives(void) {
    ui_js *js = make(NULL);
    uint32_t scope = ui_js_scope_open(js);
    double base = 10;
    ui_js_val global = ui_js_global(js);
    CHECK(ui_js_set(js, global, "sum", ui_js_function(js, "sum", 2, native_sum, &base)));
    CHECK(ui_js_set(js, global, "boom", ui_js_function(js, "boom", 0, native_throws, NULL)));
    CHECK(ui_js_set(js, global, "forgets", ui_js_function(js, "forgets", 0, native_forgets, NULL)));
    CHECK(number(js, eval(js, "sum(1, 2, 3)")) == 16);
    CHECK(equals(js, eval(js, "sum.name + sum.length"), "sum2"));
    CHECK(equals(js, eval(js, "try { boom() } catch (e) { e instanceof TypeError ? e.message : 'no' }"), "bad argument"));
    CHECK(equals(js, eval(js, "try { forgets(); 'no' } catch (e) { e.name }"), "TypeError")); // catchable
    // The native's own values are released when it returns: the stack is back where it was.
    uint32_t before = ui_js_scope_open(js);
    eval(js, "for (let i = 0; i < 1000; i++) sum(i)");
    CHECK(ui_js_scope_open(js) == before + 1);

    static int weight = 5;
    ui_js_val obj = ui_js_native_object(js, &weight, finalize);
    CHECK(ui_js_native_data(js, obj) == &weight);
    CHECK(ui_js_native_data(js, ui_js_object(js)) == NULL);
    finalized = 0;
    ui_js_scope_close(js, scope);
    ui_js_gc(js);
    CHECK_EQ_INT(finalized, 5); // collected once nothing refers to it
    ui_js_destroy(js);
}

static void test_exceptions(void) {
    ui_js *js = make(NULL);
    uint32_t scope = ui_js_scope_open(js);
    const char *src = "function inner() { throw new RangeError('too far'); }\n"
                      "function outer() { inner(); }\n"
                      "outer();\n";
    ui_js_val r;
    CHECK(!ui_js_eval_script(js, "trace.js", src, strlen(src), &r) && ui_js_is_empty(r));
    CHECK(ui_js_has_exception(js));
    ui_js_exception e;
    ui_js_val thrown;
    CHECK(ui_js_take_exception(js, &e, &thrown));
    CHECK(!ui_js_has_exception(js));
    CHECK(!strcmp(e.name, "RangeError") && !strcmp(e.message, "too far"));
    CHECK(strstr(e.stack, "inner") && strstr(e.stack, "outer") && strstr(e.stack, "trace.js:1"));
    CHECK(!e.uncatchable && ui_js_is_error(js, thrown));
    ui_js_exception_free(&e);
    // A thrown value that isn't an Error.
    CHECK(ui_js_is_empty(eval(js, "throw 42")));
    CHECK(ui_js_take_exception(js, &e, NULL) && !strcmp(e.name, "") && !strcmp(e.message, "42"));
    ui_js_exception_free(&e);
    CHECK(ui_js_is_empty(eval(js, "throw Symbol('s')")));
    CHECK(ui_js_take_exception(js, &e, NULL) && e.message[0]);
    ui_js_exception_free(&e);
    CHECK(!ui_js_take_exception(js, &e, NULL));
    // Syntax errors.
    CHECK(ui_js_is_empty(eval(js, "let = = 1")));
    CHECK(ui_js_take_exception(js, &e, NULL) && !strcmp(e.name, "SyntaxError"));
    ui_js_exception_free(&e);
    ui_js_scope_close(js, scope);
    ui_js_destroy(js);
}

static void test_refs(void) {
    ui_js *js = make(NULL);
    uint32_t scope = ui_js_scope_open(js);
    ui_js_ref *strong = ui_js_ref_new(js, eval(js, "({name: 'strong'})"), false);
    ui_js_ref *weak = ui_js_ref_new(js, eval(js, "({name: 'weak'})"), true);
    ui_js_val held = eval(js, "globalThis.held = {name: 'held'}");
    ui_js_ref *weak_held = ui_js_ref_new(js, held, true);
    CHECK(strong && weak && weak_held);
    CHECK(ui_js_ref_new(js, ui_js_number(js, 1), true) == NULL); // weak needs an object
    ui_js_scope_close(js, scope);
    ui_js_gc(js);
    scope = ui_js_scope_open(js);
    ui_js_val v, name;
    CHECK(ui_js_ref_get(js, strong, &v) && ui_js_get(js, v, "name", &name) && equals(js, name, "strong"));
    CHECK(!ui_js_ref_get(js, weak, &v) && ui_js_is_empty(v)); // collected
    CHECK(ui_js_ref_get(js, weak_held, &v) && ui_js_get(js, v, "name", &name) && equals(js, name, "held"));
    ui_js_scope_close(js, scope);
    ui_js_ref_free(js, strong);
    ui_js_ref_free(js, weak);
    ui_js_ref_free(js, weak_held);
    ui_js_destroy(js);
}

// A loader serving two modules from memory, one of them as bytecode.
typedef struct loader {
    uint8_t *bytecode;
    size_t bytecode_size;
    int loads;
} loader;

static char *resolve(void *user, const char *base, const char *specifier) {
    (void)user, (void)base;
    if (!strcmp(specifier, "./missing.js")) return NULL;
    return strdup(specifier + (specifier[0] == '.' && specifier[1] == '/' ? 2 : 0));
}

static const char *const math_src = "export const twice = (x) => 2 * x;\n";

static bool load(void *user, const char *name, ui_js_module_source *out) {
    loader *l = user;
    l->loads++;
    if (!strcmp(name, "math.js")) {
        *out = (ui_js_module_source){math_src, strlen(math_src), false};
        return true;
    }
    if (!strcmp(name, "compiled.js") && l->bytecode) {
        *out = (ui_js_module_source){l->bytecode, l->bytecode_size, true};
        return true;
    }
    return false;
}

static void test_modules(void) {
    loader l = {0};
    ui_js *js = make(&(ui_js_desc){.user = &l, .resolve = resolve, .load = load});
    uint32_t scope = ui_js_scope_open(js);
    // A host module, as `axe:host` is.
    ui_js_val exports = ui_js_object(js);
    CHECK(ui_js_set(js, exports, "answer", ui_js_number(js, 42)));
    CHECK(ui_js_define_module(js, "host:test", exports));
    CHECK(!ui_js_define_module(js, "host:test", exports));
    CHECK(ui_js_take_exception(js, NULL, NULL));

    // Bytecode compiled by one engine, evaluated by another (as the build does).
    const char *compiled = "import { twice } from './math.js';\nexport const value = twice(21);\n"
                           "globalThis.fromBytecode = value;\n";
    ui_js *builder = make(&(ui_js_desc){.user = &l, .resolve = resolve, .load = load});
    CHECK(ui_js_compile_module(builder, "compiled.js", compiled, strlen(compiled), &l.bytecode, &l.bytecode_size));
    ui_js_destroy(builder);
    CHECK(l.bytecode && l.bytecode_size > 0);
    l.loads = 0;

    const char *main_src = "import { answer } from 'host:test';\n"
                           "import { twice } from './math.js';\n"
                           "import './compiled.js';\n"
                           "globalThis.result = twice(answer);\n"
                           "await null;\n"
                           "globalThis.afterAwait = true;\n";
    ui_js_val promise;
    CHECK(ui_js_eval_module(js, "main.js", &(ui_js_module_source){main_src, strlen(main_src), false}, &promise));
    CHECK(number(js, eval(js, "result")) == 84);
    CHECK(number(js, eval(js, "fromBytecode")) == 42);
    CHECK(l.loads == 2);
    // Top-level await: the promise settles once microtasks run.
    ui_js_run_microtasks(js, NULL, NULL);
    CHECK(ui_js_promise_get(js, promise, NULL) == UI_JS_FULFILLED);
    CHECK(ui_js_truthy(js, eval(js, "afterAwait")));

    // A module that imports a missing one fails to evaluate.
    const char *bad = "import './missing.js';\n";
    CHECK(!ui_js_eval_module(js, "bad.js", &(ui_js_module_source){bad, strlen(bad), false}, &promise));
    ui_js_exception e;
    CHECK(ui_js_take_exception(js, &e, NULL) && strstr(e.message, "missing.js"));
    ui_js_exception_free(&e);
    // A module whose evaluation throws: a rejected promise.
    const char *throws = "throw new Error('at load');\n";
    CHECK(ui_js_eval_module(js, "throws.js", &(ui_js_module_source){throws, strlen(throws), false}, &promise));
    ui_js_run_microtasks(js, NULL, NULL);
    ui_js_val reason;
    CHECK(ui_js_promise_get(js, promise, &reason) == UI_JS_REJECTED && ui_js_is_error(js, reason));
    ui_js_scope_close(js, scope);
    free(l.bytecode);
    ui_js_destroy(js);
}

typedef struct rejections {
    int unhandled, handled, errors;
} rejections;

static void on_rejection(void *user, ui_js *js, ui_js_val promise, ui_js_val reason, bool handled) {
    (void)js, (void)promise, (void)reason;
    rejections *r = user;
    if (handled) r->handled++;
    else r->unhandled++;
}

static void on_job_error(void *user, ui_js *js) {
    rejections *r = user;
    r->errors++;
    ui_js_take_exception(js, NULL, NULL);
}

static void test_microtasks(void) {
    rejections r = {0};
    ui_js *js = make(&(ui_js_desc){.user = &r, .rejection = on_rejection});
    uint32_t scope = ui_js_scope_open(js);
    eval(js, "globalThis.order = [];"
             "Promise.resolve().then(() => { order.push('a'); Promise.resolve().then(() => order.push('c')); });"
             "Promise.resolve().then(() => order.push('b'));");
    CHECK(ui_js_has_microtasks(js));
    CHECK(equals(js, eval(js, "order.join('')"), "")); // nothing runs before the checkpoint
    CHECK(ui_js_run_microtasks(js, NULL, NULL) == 3);
    CHECK(equals(js, eval(js, "order.join('')"), "abc"));
    CHECK(!ui_js_has_microtasks(js));

    // queueMicrotask's path, and a job that throws: reported, the others still run.
    ui_js_val first = eval(js, "() => { order.push('d'); throw new Error('job'); }");
    ui_js_val second = eval(js, "() => order.push('e')");
    CHECK(ui_js_enqueue_microtask(js, first) && ui_js_enqueue_microtask(js, second));
    ui_js_run_microtasks(js, on_job_error, &r);
    CHECK(r.errors == 1);
    CHECK(equals(js, eval(js, "order.join('')"), "abcde"));

    // Promises from C.
    ui_js_val resolve, reject;
    ui_js_val p = ui_js_promise(js, &resolve, &reject);
    CHECK(ui_js_promise_get(js, p, NULL) == UI_JS_PENDING);
    ui_js_val out;
    CHECK(ui_js_call(js, resolve, ui_js_undefined(js), 1, (ui_js_val[]){ui_js_number(js, 9)}, &out));
    ui_js_val value;
    CHECK(ui_js_promise_get(js, p, &value) == UI_JS_FULFILLED && number(js, value) == 9);
    CHECK(ui_js_promise_get(js, ui_js_number(js, 1), NULL) == UI_JS_NOT_A_PROMISE);

    // Rejection tracking: unhandled, then handled later.
    eval(js, "globalThis.late = Promise.reject(new Error('nobody'));");
    ui_js_run_microtasks(js, NULL, NULL);
    CHECK(r.unhandled == 1 && r.handled == 0);
    eval(js, "late.catch(() => {});");
    ui_js_run_microtasks(js, NULL, NULL);
    CHECK(r.handled == 1);
    ui_js_scope_close(js, scope);
    ui_js_destroy(js);
}

static int buffer_frees;
static void free_buffer(void *user, void *data) {
    CHECK(user == &buffer_frees);
    buffer_frees++;
    free(data);
}

static void test_array_buffers(void) {
    ui_js *js = make(NULL);
    uint32_t scope = ui_js_scope_open(js);
    uint8_t *mem = calloc(1, 16);
    buffer_frees = 0;
    ui_js_val buf = ui_js_array_buffer_external(js, mem, 16, free_buffer, &buffer_frees);
    CHECK(ui_js_set(js, ui_js_global(js), "buf", buf));
    // Zero copy both ways.
    eval(js, "new Uint32Array(buf)[1] = 0xAABBCCDD;");
    CHECK(mem[4] == 0xDD && mem[7] == 0xAA);
    mem[0] = 7;
    CHECK(number(js, eval(js, "new Uint8Array(buf)[0]")) == 7);
    void *data;
    size_t size;
    CHECK(ui_js_array_buffer_data(js, buf, &data, &size) && data == mem && size == 16);
    CHECK(ui_js_view_data(js, eval(js, "new Uint8Array(buf, 4, 8)"), &data, &size) && data == mem + 4 && size == 8);
    CHECK(!ui_js_view_data(js, buf, &data, &size));
    CHECK(!ui_js_array_buffer_data(js, ui_js_object(js), &data, &size));
    // Detaching frees it through the host's function, and JS sees it empty.
    ui_js_array_buffer_detach(js, buf);
    CHECK_EQ_INT(buffer_frees, 1);
    CHECK(number(js, eval(js, "buf.byteLength")) == 0);
    // A copy.
    ui_js_val copy = ui_js_array_buffer_copy(js, "abcd", 4);
    CHECK(ui_js_array_buffer_data(js, copy, &data, &size) && size == 4 && !memcmp(data, "abcd", 4));
    ui_js_scope_close(js, scope);
    ui_js_destroy(js);
    CHECK_EQ_INT(buffer_frees, 1);
}

static int polls;
static bool stop_after(void *user) {
    (void)user;
    return ++polls > 50;
}

static void test_interrupt(void) {
    ui_js *js = make(NULL);
    uint32_t scope = ui_js_scope_open(js);
    polls = 0;
    ui_js_set_interrupt(js, stop_after, NULL);
    // Even a catch can't keep the loop going: the interrupt is uncatchable.
    CHECK(ui_js_is_empty(eval(js, "for (;;) { try { for (;;) {} } catch (e) {} }")));
    ui_js_exception e;
    CHECK(ui_js_take_exception(js, &e, NULL) && e.uncatchable);
    ui_js_exception_free(&e);
    ui_js_set_interrupt(js, NULL, NULL);
    CHECK(number(js, eval(js, "1 + 1")) == 2); // the engine still works
    ui_js_scope_close(js, scope);
    ui_js_destroy(js);
}

static void test_memory(void) {
    ui_js *js = make(&(ui_js_desc){.memory_limit = 8u << 20});
    uint32_t scope = ui_js_scope_open(js);
    ui_js_memory m;
    ui_js_memory_stats(js, &m);
    CHECK(m.used_bytes > 0 && m.limit_bytes == (8u << 20) && m.objects > 0);
    CHECK(ui_js_is_empty(eval(js, "const a = []; for (;;) a.push(new Array(10000).fill(1));")));
    ui_js_exception e;
    CHECK(ui_js_take_exception(js, &e, NULL));
    ui_js_exception_free(&e);
    ui_js_scope_close(js, scope);
    ui_js_gc(js);
    scope = ui_js_scope_open(js);
    CHECK(number(js, eval(js, "[1, 2, 3].length")) == 3); // usable again after collection
    ui_js_set_memory_limit(js, 0);
    ui_js_memory_stats(js, &m);
    CHECK(m.limit_bytes == 0);
    ui_js_scope_close(js, scope);
    ui_js_destroy(js);
    CHECK(!strcmp(ui_js_engine_name(), "quickjs-ng") && !strcmp(ui_js_engine_version(), "0.18.0"));
}

// Hot reload's needs: the same file under versioned names, loaded by import() at run time.
// (docs/ui.md, "Hot reload").
typedef struct versioned_module {
    const char *name, *source;
} versioned_module;

static const versioned_module versioned[] = {
    {"app:/a.js?v=1", "import { b } from './b.js?v=1'; export const a = 'a1' + b;\n"},
    {"app:/b.js?v=1", "export const b = 'b1'; globalThis.evals = (globalThis.evals ?? 0) + 1;\n"},
    {"app:/a.js?v=2", "import { b } from './b.js?v=1'; export const a = 'a2' + b;\n"},
    {"app:/broken.js?v=1", "throw new Error('broken at load');\n"},
    {"app:/syntax.js?v=1", "export const = 1;\n"},
    {"app:/x.js?v=1", "import { y } from './y.js?v=1'; export const x = () => 'x' + y();\n"},
    {"app:/y.js?v=1", "import { x } from './x.js?v=1'; export const y = () => 'y'; export const usesX = () => x;\n"},
    {"app:/p.js?v=1", "import { q } from './q.js?v=1'; export const p = 1;\n"},
    {"app:/q.js?v=1", "import { p } from './p.js?v=1'; export const q = p + 1;\n"},
};

static char *versioned_resolve(void *user, const char *base, const char *specifier) {
    (void)user, (void)base;
    if (!strncmp(specifier, "app:/", 5)) return strdup(specifier);
    if (specifier[0] != '.' || specifier[1] != '/') return NULL;
    char *name = malloc(strlen(specifier) + 6);
    if (name) sprintf(name, "app:/%s", specifier + 2);
    return name;
}

static bool versioned_load(void *user, const char *name, ui_js_module_source *out) {
    (*(int *)user)++;
    for (size_t i = 0; i < sizeof versioned / sizeof versioned[0]; i++) {
        if (strcmp(versioned[i].name, name)) continue;
        *out = (ui_js_module_source){versioned[i].source, strlen(versioned[i].source), false};
        return true;
    }
    return false;
}

static void test_dynamic_import(void) {
    int loads = 0;
    ui_js *js = make(&(ui_js_desc){.user = &loads, .resolve = versioned_resolve, .load = versioned_load});
    uint32_t scope = ui_js_scope_open(js);
    const char *driver = "globalThis.load = (name) => import(name);\n";
    ui_js_val promise;
    CHECK(ui_js_eval_module(js, "app:/driver.js", &(ui_js_module_source){driver, strlen(driver), false}, &promise));
    ui_js_run_microtasks(js, NULL, NULL);

    // Two versions of a file are two modules; the import they share is evaluated once.
    ui_js_val p1 = eval(js, "globalThis.p1 = load('app:/a.js?v=1').then((m) => m.a); p1");
    ui_js_run_microtasks(js, NULL, NULL);
    ui_js_val value;
    CHECK(ui_js_promise_get(js, p1, &value) == UI_JS_FULFILLED && equals(js, value, "a1b1"));
    ui_js_val p2 = eval(js, "load('app:/a.js?v=2').then((m) => m.a)");
    ui_js_run_microtasks(js, NULL, NULL);
    CHECK(ui_js_promise_get(js, p2, &value) == UI_JS_FULFILLED && equals(js, value, "a2b1"));
    CHECK(number(js, eval(js, "evals")) == 1);
    CHECK(loads == 3);

    // Importing a loaded module again gives the same namespace, with no load.
    ui_js_val same = eval(js, "Promise.all([load('app:/a.js?v=1'), load('app:/a.js?v=1')])"
                              ".then(([x, y]) => x === y)");
    ui_js_run_microtasks(js, NULL, NULL);
    CHECK(ui_js_promise_get(js, same, &value) == UI_JS_FULFILLED && ui_js_truthy(js, value));
    CHECK(loads == 3);

    // Failures reject the import's promise and leave the engine working: a throwing module, a
    // syntax error, a file the loader doesn't have, a cycle reading a binding too early.
    static const char *const failing[] = {"load('app:/broken.js?v=1')", "load('app:/syntax.js?v=1')",
                                          "load('app:/gone.js?v=1')", "load('app:/p.js?v=1')"};
    static const char *const messages[] = {"broken at load", NULL, "gone.js", "not initialized"};
    for (int i = 0; i < 4; i++) {
        ui_js_val p = eval(js, failing[i]);
        ui_js_val caught = eval(js, "(p) => p.catch(() => {})");
        ui_js_val r;
        CHECK(ui_js_call(js, caught, ui_js_undefined(js), 1, &p, &r));
        ui_js_run_microtasks(js, NULL, NULL);
        CHECK(ui_js_promise_get(js, p, &value) == UI_JS_REJECTED);
        if (messages[i]) {
            char *text = ui_js_to_utf8(js, value, NULL);
            CHECK(text && strstr(text, messages[i]));
            free(text);
        }
    }

    // A cycle links and runs.
    ui_js_val cycle = eval(js, "load('app:/x.js?v=1').then((m) => m.x())");
    ui_js_run_microtasks(js, NULL, NULL);
    CHECK(ui_js_promise_get(js, cycle, &value) == UI_JS_FULFILLED && equals(js, value, "xy"));
    ui_js_scope_close(js, scope);
    ui_js_destroy(js);
}

int main(void) {
    RUN(test_values);
    RUN(test_scopes);
    RUN(test_natives);
    RUN(test_exceptions);
    RUN(test_refs);
    RUN(test_modules);
    RUN(test_dynamic_import);
    RUN(test_microtasks);
    RUN(test_array_buffers);
    RUN(test_interrupt);
    RUN(test_memory);
    return test_failures ? 1 : 0;
}
