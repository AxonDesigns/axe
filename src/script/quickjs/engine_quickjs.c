// src/script/engine.h on QuickJS-ng: the only file that includes quickjs.h.
//
// Values: a stack of JSValues, each holding a reference. A ui_js_val is an index into it, a
// scope a mark on it; closing a scope frees the values above its mark. Index 0 is never used,
// so id 0 is the empty value.
#include "script/engine.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"

typedef struct native_module {
    char *name;
    JSValue exports;
} native_module;

struct ui_js {
    JSRuntime *rt;
    JSContext *ctx;
    JSValue *vals;
    uint32_t count, capacity;
    ui_js_desc desc;
    JSClassID native_class;
    JSValue weak_ref, weak_deref; // WeakRef and WeakRef.prototype.deref
    bool (*poll)(void *user);
    void *poll_user;
    native_module *modules;
    uint32_t module_count;
    uint32_t live_refs;
};

struct ui_js_ref {
    JSValue value; // the value, or for a weak reference a WeakRef to it
    bool weak;
};

typedef struct native_fn {
    ui_js *js;
    ui_js_native fn;
    void *data;
} native_fn;

typedef struct native_obj {
    void *data;
    void (*finalize)(void *data);
} native_obj;

typedef struct external_buffer {
    void (*free_fn)(void *user, void *data);
    void *user;
} external_buffer;

// ---- The value stack ---------------------------------------------------------------------

static JSValue get(const ui_js *js, ui_js_val v) {
    return v.id && v.id < js->count ? js->vals[v.id] : JS_UNDEFINED;
}

// Takes ownership of `v` (one reference). JS_EXCEPTION gives the empty value.
static ui_js_val push(ui_js *js, JSValue v) {
    if (JS_IsException(v)) return (ui_js_val){0};
    if (js->count == js->capacity) {
        uint32_t cap = js->capacity * 2;
        JSValue *grown = realloc(js->vals, cap * sizeof *grown);
        if (!grown) {
            JS_FreeValue(js->ctx, v);
            JS_ThrowOutOfMemory(js->ctx);
            return (ui_js_val){0};
        }
        js->vals = grown;
        js->capacity = cap;
    }
    js->vals[js->count] = v;
    return (ui_js_val){js->count++};
}

uint32_t ui_js_scope_open(ui_js *js) { return js->count; }

void ui_js_scope_close(ui_js *js, uint32_t mark) {
    while (js->count > mark) JS_FreeValue(js->ctx, js->vals[--js->count]);
}

ui_js_val ui_js_scope_escape(ui_js *js, uint32_t mark, ui_js_val v) {
    JSValue keep = v.id ? JS_DupValue(js->ctx, get(js, v)) : JS_EXCEPTION;
    ui_js_scope_close(js, mark);
    return push(js, keep);
}

// ---- Lifecycle ---------------------------------------------------------------------------

static void native_finalizer(JSRuntime *rt, JSValueConst val);

static int interrupt(JSContext *ctx, void *opaque) {
    (void)ctx;
    ui_js *js = opaque;
    return js->poll && js->poll(js->poll_user);
}

static void rejection(JSContext *ctx, JSValueConst promise, JSValueConst reason, bool handled, void *opaque) {
    (void)ctx;
    ui_js *js = opaque;
    if (!js->desc.rejection) return;
    uint32_t mark = ui_js_scope_open(js);
    ui_js_val p = push(js, JS_DupValue(js->ctx, promise)), r = push(js, JS_DupValue(js->ctx, reason));
    if (p.id && r.id) js->desc.rejection(js->desc.user, js, p, r, handled);
    ui_js_scope_close(js, mark);
}

static native_module *find_module(ui_js *js, const char *name) {
    for (uint32_t i = 0; i < js->module_count; i++)
        if (!strcmp(js->modules[i].name, name)) return &js->modules[i];
    return NULL;
}

static char *normalize(JSContext *ctx, const char *base, const char *name, void *opaque) {
    ui_js *js = opaque;
    if (find_module(js, name) || !js->desc.resolve) return js_strdup(ctx, name);
    char *resolved = js->desc.resolve(js->desc.user, base, name);
    if (!resolved) {
        JS_ThrowReferenceError(ctx, "cannot find module '%s' (imported by '%s')", name, base);
        return NULL;
    }
    char *out = js_strdup(ctx, resolved);
    free(resolved);
    return out;
}

static int native_module_init(JSContext *ctx, JSModuleDef *m) {
    JSValue exports = JS_GetModulePrivateValue(ctx, m);
    JSPropertyEnum *props;
    uint32_t n;
    int r = JS_GetOwnPropertyNames(ctx, &props, &n, exports, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY);
    for (uint32_t i = 0; r == 0 && i < n; i++) {
        const char *key = JS_AtomToCString(ctx, props[i].atom);
        if (!key) {
            r = -1;
            break;
        }
        r = JS_SetModuleExport(ctx, m, key, JS_GetProperty(ctx, exports, props[i].atom));
        JS_FreeCString(ctx, key);
    }
    if (r == 0) JS_FreePropertyEnum(ctx, props, n);
    JS_FreeValue(ctx, exports);
    return r;
}

static JSModuleDef *make_native_module(JSContext *ctx, native_module *nm) {
    JSModuleDef *m = JS_NewCModule(ctx, nm->name, native_module_init);
    if (!m) return NULL;
    JSPropertyEnum *props;
    uint32_t n;
    if (JS_GetOwnPropertyNames(ctx, &props, &n, nm->exports, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) return NULL;
    for (uint32_t i = 0; i < n; i++) {
        const char *key = JS_AtomToCString(ctx, props[i].atom);
        if (!key || JS_AddModuleExport(ctx, m, key) < 0) {
            JS_FreeCString(ctx, key);
            JS_FreePropertyEnum(ctx, props, n);
            return NULL;
        }
        JS_FreeCString(ctx, key);
    }
    JS_FreePropertyEnum(ctx, props, n);
    JS_SetModulePrivateValue(ctx, m, JS_DupValue(ctx, nm->exports));
    return m;
}

// A compiled module (from source or bytecode) as the JSModuleDef QuickJS wants back.
static JSValue compile(ui_js *js, const char *name, const ui_js_module_source *src) {
    if (src->bytecode)
        return JS_ReadObject(js->ctx, src->bytes, src->size, JS_READ_OBJ_BYTECODE);
    // JS_Eval reads up to a NUL: copy the source to terminate it.
    char *text = malloc(src->size + 1);
    if (!text) return JS_ThrowOutOfMemory(js->ctx);
    memcpy(text, src->bytes, src->size);
    text[src->size] = 0;
    JSValue v = JS_Eval(js->ctx, text, src->size, name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free(text);
    return v;
}

static JSModuleDef *load_module(JSContext *ctx, const char *name, void *opaque) {
    ui_js *js = opaque;
    native_module *nm = find_module(js, name);
    if (nm) return make_native_module(ctx, nm);
    ui_js_module_source src;
    if (!js->desc.load || !js->desc.load(js->desc.user, name, &src)) {
        JS_ThrowReferenceError(ctx, "cannot load module '%s'", name);
        return NULL;
    }
    JSValue v = compile(js, name, &src);
    if (JS_IsException(v)) return NULL;
    JSModuleDef *m = JS_VALUE_GET_PTR(v);
    JS_FreeValue(ctx, v); // the runtime holds the module
    return m;
}

ui_result ui_js_create(const ui_js_desc *desc, ui_js **out) {
    ui_js *js = calloc(1, sizeof *js);
    if (!js) return UI_ERR_OUT_OF_MEMORY;
    js->desc = *desc;
    js->capacity = 256;
    js->vals = malloc(js->capacity * sizeof *js->vals);
    js->rt = JS_NewRuntime();
    if (!js->vals || !js->rt) goto fail;
    js->vals[0] = JS_UNDEFINED;
    js->count = 1;
    JS_SetRuntimeOpaque(js->rt, js);
    if (desc->memory_limit) JS_SetMemoryLimit(js->rt, desc->memory_limit);
    if (desc->stack_limit) JS_SetMaxStackSize(js->rt, desc->stack_limit);
    JS_SetInterruptHandler(js->rt, interrupt, js);
    JS_SetHostPromiseRejectionTracker(js->rt, rejection, js);
    JS_SetModuleLoaderFunc(js->rt, normalize, load_module, js);
    JS_NewClassID(js->rt, &js->native_class);
    if (JS_NewClass(js->rt, js->native_class, &(JSClassDef){.class_name = "Native", .finalizer = native_finalizer}) < 0)
        goto fail;
    js->ctx = JS_NewContext(js->rt);
    if (!js->ctx) goto fail;
    JSValue global = JS_GetGlobalObject(js->ctx);
    js->weak_ref = JS_GetPropertyStr(js->ctx, global, "WeakRef");
    JS_FreeValue(js->ctx, global);
    JSValue proto = JS_GetPropertyStr(js->ctx, js->weak_ref, "prototype");
    js->weak_deref = JS_GetPropertyStr(js->ctx, proto, "deref");
    JS_FreeValue(js->ctx, proto);
    if (!JS_IsFunction(js->ctx, js->weak_ref) || !JS_IsFunction(js->ctx, js->weak_deref)) goto fail;
    *out = js;
    return UI_OK;
fail:
    ui_js_destroy(js);
    return UI_ERR_OUT_OF_MEMORY;
}

void ui_js_destroy(ui_js *js) {
    if (!js) return;
    if (js->ctx) {
        ui_js_scope_close(js, 1);
        JS_FreeValue(js->ctx, js->weak_ref);
        JS_FreeValue(js->ctx, js->weak_deref);
        for (uint32_t i = 0; i < js->module_count; i++) {
            JS_FreeValue(js->ctx, js->modules[i].exports);
            free(js->modules[i].name);
        }
        // Pending jobs hold values: drop them by running none; FreeContext frees the queue.
        JS_FreeContext(js->ctx);
    }
    if (js->rt) JS_FreeRuntime(js->rt);
    free(js->modules);
    free(js->vals);
    free(js);
}

void ui_js_attach_thread(ui_js *js) { JS_UpdateStackTop(js->rt); }

const char *ui_js_engine_name(void) { return "quickjs-ng"; }
const char *ui_js_engine_version(void) { return JS_GetVersion(); }

// ---- Values ------------------------------------------------------------------------------

ui_js_val ui_js_undefined(ui_js *js) { return push(js, JS_UNDEFINED); }
ui_js_val ui_js_null(ui_js *js) { return push(js, JS_NULL); }
ui_js_val ui_js_bool(ui_js *js, bool b) { return push(js, JS_NewBool(js->ctx, b)); }
ui_js_val ui_js_number(ui_js *js, double n) { return push(js, JS_NewFloat64(js->ctx, n)); }
ui_js_val ui_js_string(ui_js *js, const char *utf8, size_t len) {
    return push(js, JS_NewStringLen(js->ctx, utf8, len));
}
ui_js_val ui_js_object(ui_js *js) { return push(js, JS_NewObject(js->ctx)); }
ui_js_val ui_js_array(ui_js *js) { return push(js, JS_NewArray(js->ctx)); }
ui_js_val ui_js_global(ui_js *js) { return push(js, JS_GetGlobalObject(js->ctx)); }

ui_js_val ui_js_error(ui_js *js, ui_js_error_kind kind, const char *message) {
    JSContext *c = js->ctx;
    switch (kind) {
    case UI_JS_TYPE_ERROR: return push(js, JS_NewTypeError(c, "%s", message));
    case UI_JS_RANGE_ERROR: return push(js, JS_NewRangeError(c, "%s", message));
    case UI_JS_SYNTAX_ERROR: return push(js, JS_NewSyntaxError(c, "%s", message));
    case UI_JS_INTERNAL_ERROR: return push(js, JS_NewInternalError(c, "%s", message));
    default: return push(js, JS_NewPlainError(c, "%s", message));
    }
}

ui_js_type ui_js_typeof(ui_js *js, ui_js_val v) {
    JSValue x = get(js, v);
    if (JS_IsUndefined(x)) return UI_JS_UNDEFINED;
    if (JS_IsNull(x)) return UI_JS_NULL;
    if (JS_IsBool(x)) return UI_JS_BOOL;
    if (JS_IsNumber(x)) return UI_JS_NUMBER;
    if (JS_IsBigInt(x)) return UI_JS_BIGINT;
    if (JS_IsString(x)) return UI_JS_STRING;
    if (JS_IsSymbol(x)) return UI_JS_SYMBOL;
    return JS_IsFunction(js->ctx, x) ? UI_JS_FUNCTION : UI_JS_OBJECT;
}

bool ui_js_is_array(ui_js *js, ui_js_val v) { return JS_IsArray(get(js, v)); }
bool ui_js_is_error(ui_js *js, ui_js_val v) { return JS_IsError(get(js, v)); }
bool ui_js_strict_equal(ui_js *js, ui_js_val a, ui_js_val b) {
    return JS_IsStrictEqual(js->ctx, get(js, a), get(js, b));
}

bool ui_js_truthy(ui_js *js, ui_js_val v) { return JS_ToBool(js->ctx, get(js, v)) > 0; }

bool ui_js_to_number(ui_js *js, ui_js_val v, double *out) { return JS_ToFloat64(js->ctx, out, get(js, v)) == 0; }

char *ui_js_to_utf8(ui_js *js, ui_js_val v, size_t *len) {
    size_t n;
    const char *s = JS_ToCStringLen2(js->ctx, &n, get(js, v), false);
    if (!s) return NULL;
    char *copy = malloc(n + 1);
    if (copy) {
        memcpy(copy, s, n + 1);
        if (len) *len = n;
    } else {
        JS_ThrowOutOfMemory(js->ctx);
    }
    JS_FreeCString(js->ctx, s);
    return copy;
}

bool ui_js_get(ui_js *js, ui_js_val obj, const char *key, ui_js_val *out) {
    *out = push(js, JS_GetPropertyStr(js->ctx, get(js, obj), key));
    return out->id != 0;
}

bool ui_js_set(ui_js *js, ui_js_val obj, const char *key, ui_js_val v) {
    return JS_SetPropertyStr(js->ctx, get(js, obj), key, JS_DupValue(js->ctx, get(js, v))) >= 0;
}

bool ui_js_get_index(ui_js *js, ui_js_val obj, uint32_t i, ui_js_val *out) {
    *out = push(js, JS_GetPropertyUint32(js->ctx, get(js, obj), i));
    return out->id != 0;
}

bool ui_js_set_index(ui_js *js, ui_js_val obj, uint32_t i, ui_js_val v) {
    return JS_SetPropertyUint32(js->ctx, get(js, obj), i, JS_DupValue(js->ctx, get(js, v))) >= 0;
}

bool ui_js_define(ui_js *js, ui_js_val obj, const char *key, ui_js_val v, bool enumerable) {
    int flags = JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | (enumerable ? JS_PROP_ENUMERABLE : 0);
    return JS_DefinePropertyValueStr(js->ctx, get(js, obj), key, JS_DupValue(js->ctx, get(js, v)), flags) >= 0;
}

bool ui_js_length(ui_js *js, ui_js_val obj, uint32_t *out) {
    int64_t n;
    if (JS_GetLength(js->ctx, get(js, obj), &n) < 0) return false;
    *out = n < 0 ? 0 : n > UINT32_MAX ? UINT32_MAX : (uint32_t)n;
    return true;
}

// Arguments as JSValueConst, without taking references (the stack holds them).
#define MAX_ARGS 32

bool ui_js_call(ui_js *js, ui_js_val fn, ui_js_val this_val, uint32_t argc, const ui_js_val *argv, ui_js_val *out) {
    JSValue args[MAX_ARGS];
    if (argc > MAX_ARGS) {
        JS_ThrowRangeError(js->ctx, "more than %d arguments", MAX_ARGS);
        *out = (ui_js_val){0};
        return false;
    }
    for (uint32_t i = 0; i < argc; i++) args[i] = get(js, argv[i]);
    *out = push(js, JS_Call(js->ctx, get(js, fn), get(js, this_val), (int)argc, args));
    return out->id != 0;
}

bool ui_js_construct(ui_js *js, ui_js_val ctor, uint32_t argc, const ui_js_val *argv, ui_js_val *out) {
    JSValue args[MAX_ARGS];
    if (argc > MAX_ARGS) {
        JS_ThrowRangeError(js->ctx, "more than %d arguments", MAX_ARGS);
        *out = (ui_js_val){0};
        return false;
    }
    for (uint32_t i = 0; i < argc; i++) args[i] = get(js, argv[i]);
    *out = push(js, JS_CallConstructor(js->ctx, get(js, ctor), (int)argc, args));
    return out->id != 0;
}

// ---- Native functions and objects --------------------------------------------------------

static JSValue trampoline(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic,
                          void *opaque) {
    (void)magic;
    native_fn *nf = opaque;
    ui_js *js = nf->js;
    uint32_t mark = ui_js_scope_open(js);
    ui_js_val local[MAX_ARGS], *args = argc <= MAX_ARGS ? local : malloc((size_t)argc * sizeof *args);
    if (!args) return JS_ThrowOutOfMemory(ctx);
    ui_js_call_info call = {.this_val = push(js, JS_DupValue(ctx, this_val)), .args = args, .argc = (uint32_t)argc,
                            .data = nf->data};
    for (int i = 0; i < argc; i++) args[i] = push(js, JS_DupValue(ctx, argv[i]));
    ui_js_val r = nf->fn(js, &call);
    JSValue ret;
    if (r.id) ret = JS_DupValue(ctx, get(js, r));
    else if (JS_HasException(ctx)) ret = JS_EXCEPTION;
    else ret = JS_ThrowTypeError(ctx, "%s: no value and no exception (a host bug)", "native function");
    if (args != local) free(args);
    ui_js_scope_close(js, mark);
    return ret;
}

static void free_native_fn(void *opaque) { free(opaque); }

ui_js_val ui_js_function(ui_js *js, const char *name, uint32_t length, ui_js_native fn, void *data) {
    native_fn *nf = malloc(sizeof *nf);
    if (!nf) return push(js, JS_ThrowOutOfMemory(js->ctx));
    *nf = (native_fn){js, fn, data};
    JSValue f = JS_NewCClosure(js->ctx, trampoline, name, free_native_fn, (int)length, 0, nf);
    if (JS_IsException(f)) free(nf);
    return push(js, f);
}

static void native_finalizer(JSRuntime *rt, JSValueConst val) {
    ui_js *js = JS_GetRuntimeOpaque(rt);
    native_obj *o = JS_GetOpaque(val, js->native_class);
    if (!o) return;
    if (o->finalize) o->finalize(o->data);
    free(o);
}

ui_js_val ui_js_native_object(ui_js *js, void *data, void (*finalize)(void *data)) {
    native_obj *o = malloc(sizeof *o);
    if (!o) return push(js, JS_ThrowOutOfMemory(js->ctx));
    *o = (native_obj){data, finalize};
    JSValue obj = JS_NewObjectClass(js->ctx, (int)js->native_class);
    if (JS_IsException(obj)) {
        free(o);
        return (ui_js_val){0};
    }
    JS_SetOpaque(obj, o);
    return push(js, obj);
}

void *ui_js_native_data(ui_js *js, ui_js_val obj) {
    native_obj *o = JS_GetOpaque(get(js, obj), js->native_class);
    return o ? o->data : NULL;
}

// ---- Exceptions --------------------------------------------------------------------------

ui_js_val ui_js_throw(ui_js *js, ui_js_val v) {
    JS_Throw(js->ctx, JS_DupValue(js->ctx, get(js, v)));
    return (ui_js_val){0};
}

ui_js_val ui_js_throw_error(ui_js *js, ui_js_error_kind kind, const char *fmt, ...) {
    char message[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof message, fmt, ap);
    va_end(ap);
    ui_js_val e = ui_js_error(js, kind, message);
    if (e.id) ui_js_throw(js, e);
    return (ui_js_val){0};
}

bool ui_js_has_exception(ui_js *js) { return JS_HasException(js->ctx); }

// A property of an error as a string; "" when absent or when reading it throws.
static char *string_property(ui_js *js, JSValueConst obj, const char *key) {
    JSValue v = JS_GetPropertyStr(js->ctx, obj, key);
    char *out = NULL;
    if (JS_IsException(v)) {
        JS_FreeValue(js->ctx, JS_GetException(js->ctx));
    } else if (!JS_IsUndefined(v)) {
        const char *s = JS_ToCString(js->ctx, v);
        if (s) {
            out = strdup(s);
            JS_FreeCString(js->ctx, s);
        } else {
            JS_FreeValue(js->ctx, JS_GetException(js->ctx));
        }
    }
    JS_FreeValue(js->ctx, v);
    return out ? out : strdup("");
}

static void describe(ui_js *js, JSValueConst v, ui_js_exception *out) {
    *out = (ui_js_exception){.uncatchable = JS_IsUncatchableError(v)};
    if (JS_IsError(v)) {
        out->name = string_property(js, v, "name");
        out->message = string_property(js, v, "message");
        out->stack = string_property(js, v, "stack");
        return;
    }
    out->name = strdup("");
    out->stack = strdup("");
    const char *s = JS_ToCString(js->ctx, v);
    if (s) {
        out->message = strdup(s);
        JS_FreeCString(js->ctx, s);
    } else {
        JS_FreeValue(js->ctx, JS_GetException(js->ctx)); // a symbol: String() would work, ToString throws
        out->message = strdup("(a value that can't be converted to a string)");
    }
}

bool ui_js_take_exception(ui_js *js, ui_js_exception *out, ui_js_val *value) {
    if (!JS_HasException(js->ctx)) return false;
    JSValue e = JS_GetException(js->ctx);
    if (out) describe(js, e, out);
    if (value) *value = push(js, e);
    else JS_FreeValue(js->ctx, e);
    return true;
}

void ui_js_describe(ui_js *js, ui_js_val v, ui_js_exception *out) { describe(js, get(js, v), out); }

void ui_js_exception_free(ui_js_exception *e) {
    free(e->name);
    free(e->message);
    free(e->stack);
    *e = (ui_js_exception){0};
}

// ---- References --------------------------------------------------------------------------

ui_js_ref *ui_js_ref_new(ui_js *js, ui_js_val v, bool weak) {
    JSValue x = get(js, v);
    if (weak && !JS_IsObject(x)) return NULL;
    ui_js_ref *r = malloc(sizeof *r);
    if (!r) return NULL;
    r->weak = weak;
    if (weak) {
        JSValue target = x;
        r->value = JS_CallConstructor(js->ctx, js->weak_ref, 1, &target);
        if (JS_IsException(r->value)) {
            JS_FreeValue(js->ctx, JS_GetException(js->ctx));
            free(r);
            return NULL;
        }
    } else {
        r->value = JS_DupValue(js->ctx, x);
    }
    js->live_refs++;
    return r;
}

bool ui_js_ref_get(ui_js *js, const ui_js_ref *r, ui_js_val *out) {
    if (!r->weak) {
        *out = push(js, JS_DupValue(js->ctx, r->value));
        return out->id != 0;
    }
    JSValue v = JS_Call(js->ctx, js->weak_deref, r->value, 0, NULL);
    if (JS_IsException(v) || JS_IsUndefined(v)) {
        if (JS_IsException(v)) JS_FreeValue(js->ctx, JS_GetException(js->ctx));
        *out = (ui_js_val){0};
        return false;
    }
    *out = push(js, v);
    return out->id != 0;
}

void ui_js_ref_free(ui_js *js, ui_js_ref *r) {
    if (!r) return;
    JS_FreeValue(js->ctx, r->value);
    js->live_refs--;
    free(r);
}

// ---- Modules and scripts -----------------------------------------------------------------

bool ui_js_define_module(ui_js *js, const char *name, ui_js_val exports) {
    if (find_module(js, name)) {
        JS_ThrowTypeError(js->ctx, "module '%s' is already defined", name);
        return false;
    }
    native_module *grown = realloc(js->modules, (js->module_count + 1) * sizeof *grown);
    char *copy = strdup(name);
    if (!grown || !copy) {
        if (grown) js->modules = grown;
        free(copy);
        JS_ThrowOutOfMemory(js->ctx);
        return false;
    }
    js->modules = grown;
    js->modules[js->module_count++] = (native_module){copy, JS_DupValue(js->ctx, get(js, exports))};
    return true;
}

bool ui_js_eval_module(ui_js *js, const char *name, const ui_js_module_source *src, ui_js_val *result) {
    JSValue m = compile(js, name, src);
    if (JS_IsException(m)) {
        *result = (ui_js_val){0};
        return false;
    }
    if (src->bytecode && JS_ResolveModule(js->ctx, m) < 0) {
        JS_FreeValue(js->ctx, m);
        *result = (ui_js_val){0};
        return false;
    }
    *result = push(js, JS_EvalFunction(js->ctx, m)); // takes m
    return result->id != 0;
}

bool ui_js_compile_module(ui_js *js, const char *name, const char *src, size_t len, uint8_t **out, size_t *size) {
    JSValue m = compile(js, name, &(ui_js_module_source){src, len, false});
    if (JS_IsException(m)) return false;
    // Debug information stays (stack traces need its line table); the source text goes.
    uint8_t *bytes = JS_WriteObject(js->ctx, size, m, JS_WRITE_OBJ_BYTECODE | JS_WRITE_OBJ_STRIP_SOURCE);
    JS_FreeValue(js->ctx, m);
    if (!bytes) return false;
    *out = malloc(*size ? *size : 1);
    if (*out) memcpy(*out, bytes, *size);
    js_free(js->ctx, bytes);
    if (!*out) JS_ThrowOutOfMemory(js->ctx);
    return *out != NULL;
}

bool ui_js_eval_script(ui_js *js, const char *name, const char *src, size_t len, ui_js_val *result) {
    char *text = malloc(len + 1);
    if (!text) {
        JS_ThrowOutOfMemory(js->ctx);
        *result = (ui_js_val){0};
        return false;
    }
    memcpy(text, src, len);
    text[len] = 0;
    *result = push(js, JS_Eval(js->ctx, text, len, name, JS_EVAL_TYPE_GLOBAL));
    free(text);
    return result->id != 0;
}

// ---- Microtasks and promises -------------------------------------------------------------

uint32_t ui_js_run_microtasks(ui_js *js, void (*on_error)(void *user, ui_js *js), void *user) {
    uint32_t ran = 0;
    JSContext *ctx;
    while (JS_IsJobPending(js->rt)) {
        int r = JS_ExecutePendingJob(js->rt, &ctx);
        if (r == 0) break;
        ran++;
        if (r < 0) {
            if (on_error) on_error(user, js);
            if (JS_HasException(js->ctx)) JS_FreeValue(js->ctx, JS_GetException(js->ctx));
        }
    }
    return ran;
}

bool ui_js_has_microtasks(ui_js *js) { return JS_IsJobPending(js->rt); }

static JSValue call_job(JSContext *ctx, int argc, JSValueConst *argv) {
    (void)argc;
    return JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);
}

bool ui_js_enqueue_microtask(ui_js *js, ui_js_val fn) {
    JSValue f = get(js, fn);
    return JS_EnqueueJob(js->ctx, call_job, 1, &f) == 0;
}

ui_js_val ui_js_promise(ui_js *js, ui_js_val *resolve, ui_js_val *reject) {
    JSValue funcs[2];
    JSValue p = JS_NewPromiseCapability(js->ctx, funcs);
    if (JS_IsException(p)) return (ui_js_val){0};
    ui_js_val out = push(js, p);
    *resolve = push(js, funcs[0]);
    *reject = push(js, funcs[1]);
    return out.id && resolve->id && reject->id ? out : (ui_js_val){0};
}

ui_js_promise_state ui_js_promise_get(ui_js *js, ui_js_val promise, ui_js_val *result) {
    JSValue p = get(js, promise);
    if (!JS_IsPromise(p)) return UI_JS_NOT_A_PROMISE;
    switch (JS_PromiseState(js->ctx, p)) {
    case JS_PROMISE_FULFILLED:
        if (result) *result = push(js, JS_PromiseResult(js->ctx, p));
        return UI_JS_FULFILLED;
    case JS_PROMISE_REJECTED:
        if (result) *result = push(js, JS_PromiseResult(js->ctx, p));
        return UI_JS_REJECTED;
    default: return UI_JS_PENDING;
    }
}

// ---- Binary data -------------------------------------------------------------------------

// QuickJS frees (or would resize) an external buffer through this: size 0 frees.
static void *external_realloc(JSRuntime *rt, void *opaque, void *ptr, size_t size) {
    (void)rt;
    external_buffer *e = opaque;
    if (size) return NULL; // external buffers keep their size
    if (e) {
        if (e->free_fn) e->free_fn(e->user, ptr);
        free(e);
    }
    return NULL;
}

ui_js_val ui_js_array_buffer_external(ui_js *js, void *data, size_t size,
                                      void (*free_fn)(void *user, void *data), void *user) {
    external_buffer *e = malloc(sizeof *e);
    if (!e) return push(js, JS_ThrowOutOfMemory(js->ctx));
    *e = (external_buffer){free_fn, user};
    JSValue v = JS_NewArrayBuffer(js->ctx, data, size, 0, external_realloc, e, false);
    if (JS_IsException(v)) free(e);
    return push(js, v);
}

ui_js_val ui_js_array_buffer_copy(ui_js *js, const void *data, size_t size) {
    return push(js, JS_NewArrayBufferCopy(js->ctx, data, size));
}

bool ui_js_array_buffer_data(ui_js *js, ui_js_val v, void **data, size_t *size) {
    JSValue x = get(js, v);
    if (!JS_IsArrayBuffer(x)) return false;
    uint8_t *p = JS_GetArrayBuffer(js->ctx, size, x);
    if (!p) {
        if (JS_HasException(js->ctx)) JS_FreeValue(js->ctx, JS_GetException(js->ctx));
        *size = 0;
    }
    *data = p;
    return p != NULL || *size == 0;
}

bool ui_js_view_data(ui_js *js, ui_js_val v, void **data, size_t *size) {
    JSValue x = get(js, v);
    if (JS_GetTypedArrayType(x) < 0) return false;
    size_t offset, length, per;
    JSValue buf = JS_GetTypedArrayBuffer(js->ctx, x, &offset, &length, &per);
    if (JS_IsException(buf)) {
        JS_FreeValue(js->ctx, JS_GetException(js->ctx));
        return false;
    }
    size_t total;
    uint8_t *p = JS_GetArrayBuffer(js->ctx, &total, buf);
    JS_FreeValue(js->ctx, buf);
    if (!p) {
        if (JS_HasException(js->ctx)) JS_FreeValue(js->ctx, JS_GetException(js->ctx));
        *data = NULL;
        *size = 0;
        return true; // detached: empty
    }
    *data = p + offset;
    *size = length;
    return true;
}

void ui_js_array_buffer_detach(ui_js *js, ui_js_val v) {
    if (JS_IsArrayBuffer(get(js, v))) JS_DetachArrayBuffer(js->ctx, get(js, v));
}

// ---- Limits and diagnostics --------------------------------------------------------------

void ui_js_set_interrupt(ui_js *js, bool (*poll)(void *user), void *user) {
    js->poll = poll;
    js->poll_user = user;
}

void ui_js_set_memory_limit(ui_js *js, size_t bytes) { JS_SetMemoryLimit(js->rt, bytes ? bytes : (size_t)-1); }

void ui_js_memory_stats(ui_js *js, ui_js_memory *out) {
    JSMemoryUsage m;
    JS_ComputeMemoryUsage(js->rt, &m);
    *out = (ui_js_memory){
        .used_bytes = (uint64_t)m.malloc_size,
        .limit_bytes = m.malloc_limit < 0 || (uint64_t)m.malloc_limit >= (uint64_t)INT64_MAX ? 0 : (uint64_t)m.malloc_limit,
        .allocations = (uint64_t)m.malloc_count,
        .objects = (uint64_t)m.obj_count,
        .strings = (uint64_t)m.str_count,
        .string_bytes = (uint64_t)m.str_size,
        .functions = (uint64_t)m.js_func_count,
        .bytecode_bytes = (uint64_t)m.js_func_code_size,
        .array_buffer_bytes = (uint64_t)m.binary_object_size,
    };
}

void ui_js_gc(ui_js *js) { JS_RunGC(js->rt); }
