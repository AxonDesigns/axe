// Hot reload's host side (dev.h, docs/ui.md "Hot reload").
#include "script/host/dev.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#define APP_PREFIX "app:/"
#define PKG_PREFIX "pkg:/"
#define MAP_MARKER "//# sourceMappingURL=data:application/json;base64,"
#define MAPS_KEPT 4u

struct ui_dev_channel {
    mtx_t lock;
    ui_dev_batch *head, *tail;
    void (*wake)(void *);
    void *wake_user;
    char *entry, *packages;
};

ui_result ui_dev_channel_create(const ui_dev_desc *desc, ui_dev_channel **out) {
    ui_dev_channel *ch = calloc(1, sizeof *ch);
    if (!ch) return UI_ERR_OUT_OF_MEMORY;
    ch->entry = strdup(desc->entry);
    ch->packages = strdup(desc->packages);
    if (!ch->entry || !ch->packages || mtx_init(&ch->lock, mtx_plain) != thrd_success) {
        free(ch->entry);
        free(ch->packages);
        free(ch);
        return UI_ERR_OUT_OF_MEMORY;
    }
    *out = ch;
    return UI_OK;
}

void ui_dev_batch_free(ui_dev_batch *b) {
    if (!b) return;
    for (uint32_t i = 0; i < b->count; i++) {
        free(b->files[i].path);
        free(b->files[i].code);
        free(b->files[i].error);
    }
    free(b->files);
    free(b);
}

void ui_dev_channel_destroy(ui_dev_channel *ch) {
    if (!ch) return;
    while (ch->head) {
        ui_dev_batch *next = ch->head->next;
        ui_dev_batch_free(ch->head);
        ch->head = next;
    }
    mtx_destroy(&ch->lock);
    free(ch->entry);
    free(ch->packages);
    free(ch);
}

void ui_dev_channel_set_wake(ui_dev_channel *ch, void (*wake)(void *user), void *user) {
    mtx_lock(&ch->lock);
    ch->wake = wake;
    ch->wake_user = user;
    mtx_unlock(&ch->lock);
}

void ui_dev_channel_post(ui_dev_channel *ch, ui_dev_batch *batch) {
    batch->next = NULL;
    mtx_lock(&ch->lock);
    if (ch->tail) ch->tail->next = batch;
    else ch->head = batch;
    ch->tail = batch;
    void (*wake)(void *) = ch->wake;
    void *user = ch->wake_user;
    mtx_unlock(&ch->lock);
    if (wake) wake(user);
}

static ui_dev_batch *channel_take(ui_dev_channel *ch) {
    mtx_lock(&ch->lock);
    ui_dev_batch *b = ch->head;
    ch->head = ch->tail = NULL;
    mtx_unlock(&ch->lock);
    return b;
}

// ---- The module table --------------------------------------------------------------------

typedef struct dev_map {
    uint32_t version;
    char *json;
} dev_map;

typedef struct dev_module {
    char *path;
    char *code;       // the newest build, kept: a reverted update imports it again
    size_t size;
    uint32_t version; // what imports resolve to; 0: not evaluated yet
    uint32_t staged;  // the version an update is importing; 0: none
    uint32_t last;    // the newest number used: a name is never reused
    bool deleted;
    dev_map maps[MAPS_KEPT]; // by version: stacks can come from an older one
    uint32_t next_map;
} dev_module;

typedef struct dev_edge {
    uint32_t from, version, to;
} dev_edge;

typedef struct dev_change {
    uint32_t module;
    char *error;
    bool deleted;
} dev_change;

struct ui_dev {
    ui_dev_channel *ch;
    ui_js *js;
    ui_js_ref *handler;
    dev_module *modules;
    uint32_t module_count;
    dev_edge *edges;
    uint32_t edge_count;
    dev_change *changes;
    uint32_t change_count;
    uint64_t event_ns;
    bool updating;      // between hotBegin and hotCommit or hotRevert
    char *package_code; // what the last package load returned
};

static dev_module *find(ui_dev *d, const char *path, size_t len) {
    for (uint32_t i = 0; i < d->module_count; i++)
        if (strlen(d->modules[i].path) == len && !memcmp(d->modules[i].path, path, len)) return &d->modules[i];
    return NULL;
}

static dev_module *find_live(ui_dev *d, const char *path, size_t len) {
    dev_module *m = find(d, path, len);
    return m && !m->deleted ? m : NULL;
}

static dev_module *add_module(ui_dev *d, const char *path) {
    dev_module *grown = realloc(d->modules, (d->module_count + 1) * sizeof *grown);
    if (!grown) return NULL;
    d->modules = grown;
    dev_module *m = &d->modules[d->module_count];
    *m = (dev_module){.path = strdup(path)};
    if (!m->path) return NULL;
    d->module_count++;
    return m;
}

ui_result ui_dev_create(ui_dev_channel *ch, ui_js *js, ui_dev **out) {
    ui_dev *d = calloc(1, sizeof *d);
    if (!d) return UI_ERR_OUT_OF_MEMORY;
    d->ch = ch;
    d->js = js;
    *out = d;
    return UI_OK;
}

void ui_dev_release(ui_dev *d) {
    if (!d) return;
    ui_js_ref_free(d->js, d->handler);
    d->handler = NULL;
}

void ui_dev_destroy(ui_dev *d) {
    if (!d) return;
    for (uint32_t i = 0; i < d->module_count; i++) {
        free(d->modules[i].path);
        free(d->modules[i].code);
        for (uint32_t m = 0; m < MAPS_KEPT; m++) free(d->modules[i].maps[m].json);
    }
    for (uint32_t i = 0; i < d->change_count; i++) free(d->changes[i].error);
    free(d->changes);
    free(d->package_code);
    free(d->modules);
    free(d->edges);
    free(d);
}

static void add_edge(ui_dev *d, uint32_t from, uint32_t version, uint32_t to) {
    for (uint32_t i = 0; i < d->edge_count; i++)
        if (d->edges[i].from == from && d->edges[i].version == version && d->edges[i].to == to) return;
    dev_edge *grown = realloc(d->edges, (d->edge_count + 1) * sizeof *grown);
    if (!grown) return;
    d->edges = grown;
    d->edges[d->edge_count++] = (dev_edge){from, version, to};
}

// Drops the edges of versions that aren't a module's live one.
static void prune_edges(ui_dev *d) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < d->edge_count; i++)
        if (d->edges[i].version == d->modules[d->edges[i].from].version) d->edges[kept++] = d->edges[i];
    d->edge_count = kept;
}

// ---- Names ---------------------------------------------------------------------------------

// "app:/dir/a.tsx?v=2" into its path and version; false for another kind of name.
static bool parse_name(const char *name, const char **path, size_t *len, uint32_t *version) {
    if (strncmp(name, APP_PREFIX, strlen(APP_PREFIX))) return false;
    *path = name + strlen(APP_PREFIX);
    const char *q = strstr(*path, "?v=");
    *len = q ? (size_t)(q - *path) : strlen(*path);
    *version = q ? (uint32_t)strtoul(q + 3, NULL, 10) : 0;
    return true;
}

static char *versioned_name(const dev_module *m, uint32_t version) {
    size_t n = strlen(APP_PREFIX) + strlen(m->path) + 16;
    char *s = malloc(n);
    if (s) snprintf(s, n, APP_PREFIX "%s?v=%u", m->path, version);
    return s;
}

// base's directory joined with a relative specifier, with . and .. folded. NULL if it leaves the
// root, which is what the build's rule forbids (docs/ui.md, "Building").
static char *join(const char *base, size_t base_len, const char *rel) {
    size_t n = base_len + strlen(rel) + 2;
    char *out = malloc(n);
    if (!out) return NULL;
    size_t len = base_len;
    while (len && base[len - 1] != '/') len--;
    memcpy(out, base, len);
    while (*rel) {
        const char *slash = strchr(rel, '/');
        size_t part = slash ? (size_t)(slash - rel) : strlen(rel);
        if (part == 2 && !memcmp(rel, "..", 2)) {
            if (!len) {
                free(out);
                return NULL;
            }
            len--;
            while (len && out[len - 1] != '/') len--;
        } else if (part && !(part == 1 && rel[0] == '.')) {
            memcpy(out + len, rel, part);
            len += part;
            if (slash) out[len++] = '/';
        }
        rel += part + (slash ? 1 : 0);
    }
    out[len] = 0;
    return out;
}

// What an import of `path` means, trying what TypeScript's resolution would.
static dev_module *resolve_file(ui_dev *d, const char *path) {
    static const char *const suffixes[] = {"", ".tsx", ".ts", ".js", "/index.tsx", "/index.ts"};
    size_t len = strlen(path);
    char *candidate = malloc(len + 16);
    if (!candidate) return NULL;
    dev_module *found = NULL;
    for (size_t i = 0; !found && i < sizeof suffixes / sizeof suffixes[0]; i++) {
        snprintf(candidate, len + 16, "%s%s", path, suffixes[i]);
        found = find_live(d, candidate, strlen(candidate));
    }
    // TypeScript lets `./x.js` name x.ts.
    if (!found && len > 3 && !strcmp(path + len - 3, ".js")) {
        for (size_t i = 1; !found && i < 3; i++) {
            snprintf(candidate, len + 16, "%.*s%s", (int)(len - 3), path, suffixes[i]);
            found = find_live(d, candidate, strlen(candidate));
        }
    }
    free(candidate);
    return found;
}

// @axe packages are prebuilt with code splitting: one shared chunk behind an entry each, so
// their state is single however many specifiers name it.
static const char *package_entry(const char *specifier) {
    static const struct {
        const char *specifier, *file;
    } packages[] = {
        {"@axe/ui", "ui.js"},       {"@axe/ui/jsx-runtime", "jsx-runtime.js"},
        {"@axe/ui/hot", "hot.js"},  {"@axe/core", "core.js"},
        {"@axe/core/internal", "internal.js"}, {"@axe/wire", "wire.js"},
        {"@axe/testing", "testing.js"},
    };
    for (size_t i = 0; i < sizeof packages / sizeof packages[0]; i++)
        if (!strcmp(packages[i].specifier, specifier)) return packages[i].file;
    return NULL;
}

static char *pkg_name(const char *file) {
    char *s = malloc(strlen(PKG_PREFIX) + strlen(file) + 1);
    if (s) sprintf(s, PKG_PREFIX "%s", file);
    return s;
}

char *ui_dev_resolve(void *dev, const char *base, const char *specifier) {
    ui_dev *d = dev;
    if (!strncmp(specifier, APP_PREFIX, strlen(APP_PREFIX)) || !strncmp(specifier, PKG_PREFIX, strlen(PKG_PREFIX)))
        return strdup(specifier);
    const char *package = package_entry(specifier);
    if (package) return pkg_name(package);

    bool relative = specifier[0] == '.' && (specifier[1] == '/' || (specifier[1] == '.' && specifier[2] == '/'));
    if (!relative) return NULL;
    // A package's chunks import each other.
    if (!strncmp(base, PKG_PREFIX, strlen(PKG_PREFIX))) {
        const char *rel = specifier + 2;
        return pkg_name(rel);
    }
    const char *base_path;
    size_t base_len;
    uint32_t base_version;
    if (!parse_name(base, &base_path, &base_len, &base_version)) return NULL;
    char *path = join(base_path, base_len, specifier);
    if (!path) return NULL;
    dev_module *m = resolve_file(d, path);
    free(path);
    if (!m) return NULL;
    uint32_t to = (uint32_t)(m - d->modules);
    dev_module *from = find(d, base_path, base_len);
    if (from) add_edge(d, (uint32_t)(from - d->modules), base_version, to);
    // A file first imported now: staged with the update that brought it in, so a revert forgets it.
    if (!m->version && !m->staged) {
        if (d->updating) m->staged = ++m->last;
        else m->version = ++m->last;
    }
    return versioned_name(m, m->staged ? m->staged : m->version);
}

static char *read_package(ui_dev *d, const char *file, size_t *size) {
    size_t n = strlen(d->ch->packages) + strlen(file) + 2;
    char *path = malloc(n);
    if (!path) return NULL;
    snprintf(path, n, "%s/%s", d->ch->packages, file);
    FILE *f = fopen(path, "rb");
    free(path);
    if (!f) return NULL;
    char *data = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long len = ftell(f);
        if (len >= 0 && fseek(f, 0, SEEK_SET) == 0 && (data = malloc((size_t)len + 1))) {
            if (fread(data, 1, (size_t)len, f) != (size_t)len) {
                free(data);
                data = NULL;
            } else {
                *size = (size_t)len;
            }
        }
    }
    fclose(f);
    return data;
}

// ---- Source maps ---------------------------------------------------------------------------

static int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// The module's inline source map, as JSON. NULL if it has none.
static char *inline_map(const char *code, size_t size) {
    const char *at = NULL;
    size_t marker = strlen(MAP_MARKER);
    for (size_t i = size > marker ? size - marker : 0; i-- > 0;) {
        if (!memcmp(code + i, MAP_MARKER, marker)) {
            at = code + i + marker;
            break;
        }
    }
    if (!at) return NULL;
    size_t avail = size - (size_t)(at - code);
    char *out = malloc(avail / 4 * 3 + 4);
    if (!out) return NULL;
    size_t len = 0;
    uint32_t bits = 0;
    int have = 0;
    for (size_t i = 0; i < avail; i++) {
        int v = base64_value(at[i]);
        if (v < 0) break;
        bits = (bits << 6) | (uint32_t)v;
        have += 6;
        if (have >= 8) {
            have -= 8;
            out[len++] = (char)((bits >> have) & 0xFF);
        }
    }
    out[len] = 0;
    return out;
}

static void remember_map(dev_module *m, uint32_t version) {
    for (uint32_t i = 0; i < MAPS_KEPT; i++)
        if (m->maps[i].version == version) return;
    dev_map *slot = &m->maps[m->next_map];
    m->next_map = (m->next_map + 1) % MAPS_KEPT;
    free(slot->json);
    *slot = (dev_map){version, inline_map(m->code, m->size)};
}

bool ui_dev_load(void *dev, const char *name, ui_js_module_source *out) {
    ui_dev *d = dev;
    if (!strncmp(name, PKG_PREFIX, strlen(PKG_PREFIX))) {
        size_t size = 0;
        char *code = read_package(d, name + strlen(PKG_PREFIX), &size);
        if (!code) return false;
        free(d->package_code); // only the load call reads it
        d->package_code = code;
        *out = (ui_js_module_source){code, size, false};
        return true;
    }
    const char *path;
    size_t len;
    uint32_t version;
    if (!parse_name(name, &path, &len, &version)) return false;
    dev_module *m = find_live(d, path, len);
    if (!m || !m->code) return false;
    remember_map(m, version);
    *out = (ui_js_module_source){m->code, m->size, false};
    return true;
}

// ---- Batches -------------------------------------------------------------------------------

static void add_change(ui_dev *d, uint32_t module, char *error, bool deleted) {
    dev_change *grown = realloc(d->changes, (d->change_count + 1) * sizeof *grown);
    if (!grown) {
        free(error);
        return;
    }
    d->changes = grown;
    d->changes[d->change_count++] = (dev_change){module, error, deleted};
}

bool ui_dev_take(ui_dev *d) {
    ui_dev_batch *b = channel_take(d->ch);
    if (!b) return false;
    while (b) {
        for (uint32_t i = 0; i < b->count; i++) {
            ui_dev_file *f = &b->files[i];
            dev_module *m = find(d, f->path, strlen(f->path));
            if (!m && !(m = add_module(d, f->path))) continue;
            if (f->deleted) {
                m->deleted = true;
                free(m->code);
                m->code = NULL;
            } else if (f->code) {
                m->deleted = false;
                free(m->code);
                m->code = f->code;
                m->size = f->size;
                f->code = NULL;
            }
            add_change(d, (uint32_t)(m - d->modules), f->error, f->deleted);
            f->error = NULL;
        }
        if (b->event_ns && !d->event_ns) d->event_ns = b->event_ns;
        ui_dev_batch *next = b->next;
        ui_dev_batch_free(b);
        b = next;
    }
    return true;
}

uint64_t ui_dev_take_event_ns(ui_dev *d) {
    uint64_t t = d->event_ns;
    d->event_ns = 0;
    return t;
}

bool ui_dev_deliver(ui_dev *d) {
    ui_js *js = d->js;
    ui_js_val fn, list = ui_js_array(js), r;
    bool ok = true;
    for (uint32_t i = 0; ok && i < d->change_count; i++) {
        dev_change *c = &d->changes[i];
        ui_js_val o = ui_js_object(js);
        const char *path = d->modules[c->module].path;
        ok = ui_js_set(js, o, "file", ui_js_string(js, path, strlen(path))) &&
             ui_js_set(js, o, "deleted", ui_js_bool(js, c->deleted)) &&
             (!c->error || ui_js_set(js, o, "error", ui_js_string(js, c->error, strlen(c->error)))) &&
             ui_js_set_index(js, list, i, o);
    }
    for (uint32_t i = 0; i < d->change_count; i++) free(d->changes[i].error);
    d->change_count = 0;
    if (!ok) return false;
    if (!d->handler || !ui_js_ref_get(js, d->handler, &fn)) return true; // the app hasn't loaded @axe/ui
    return ui_js_call(js, fn, ui_js_undefined(js), 1, &list, &r);
}

// ---- The host API ---------------------------------------------------------------------------

static ui_js_val native_set_handler(ui_js *js, const ui_js_call_info *call) {
    ui_dev *d = call->data;
    if (call->argc < 1) return ui_js_undefined(js);
    ui_js_ref_free(js, d->handler);
    d->handler = ui_js_ref_new(js, call->args[0], false);
    return ui_js_undefined(js);
}

// hotBegin(files): stages a new version of each, and returns their names to import.
static ui_js_val native_begin(ui_js *js, const ui_js_call_info *call) {
    ui_dev *d = call->data;
    d->updating = true;
    ui_js_val out = ui_js_array(js);
    uint32_t n = 0;
    if (call->argc < 1 || !ui_js_length(js, call->args[0], &n)) return out;
    // All are staged before any is named: a module's imports must see every new version.
    for (uint32_t pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < n; i++) {
            ui_js_val item;
            if (!ui_js_get_index(js, call->args[0], i, &item)) return (ui_js_val){0};
            char *path = ui_js_to_utf8(js, item, NULL);
            if (!path) return (ui_js_val){0};
            dev_module *m = find_live(d, path, strlen(path));
            free(path);
            if (!m) continue;
            if (pass == 0) {
                if (!m->staged) m->staged = ++m->last;
                continue;
            }
            char *name = versioned_name(m, m->staged);
            if (!name) continue;
            ui_js_set_index(js, out, i, ui_js_string(js, name, strlen(name)));
            free(name);
        }
    }
    return out;
}

static ui_js_val native_commit(ui_js *js, const ui_js_call_info *call) {
    ui_dev *d = call->data;
    for (uint32_t i = 0; i < d->module_count; i++) {
        if (!d->modules[i].staged) continue;
        d->modules[i].version = d->modules[i].staged;
        d->modules[i].staged = 0;
    }
    prune_edges(d);
    d->updating = false;
    return ui_js_undefined(js);
}

static ui_js_val native_revert(ui_js *js, const ui_js_call_info *call) {
    ui_dev *d = call->data;
    for (uint32_t i = 0; i < d->module_count; i++) d->modules[i].staged = 0;
    prune_edges(d);
    d->updating = false;
    return ui_js_undefined(js);
}

// hotImporters(file): the files whose live version imports it.
static ui_js_val native_importers(ui_js *js, const ui_js_call_info *call) {
    ui_dev *d = call->data;
    ui_js_val out = ui_js_array(js);
    char *path = call->argc ? ui_js_to_utf8(js, call->args[0], NULL) : NULL;
    if (!path) return call->argc ? (ui_js_val){0} : out;
    dev_module *m = find(d, path, strlen(path));
    free(path);
    if (!m) return out;
    uint32_t to = (uint32_t)(m - d->modules), n = 0;
    for (uint32_t i = 0; i < d->edge_count; i++) {
        dev_edge *e = &d->edges[i];
        dev_module *from = &d->modules[e->from];
        if (e->to != to || e->version != from->version || from->deleted) continue;
        ui_js_set_index(js, out, n++, ui_js_string(js, from->path, strlen(from->path)));
    }
    return out;
}

// hotSourceMap(name): the JSON of a versioned module's source map, or undefined.
static ui_js_val native_source_map(ui_js *js, const ui_js_call_info *call) {
    ui_dev *d = call->data;
    char *name = call->argc ? ui_js_to_utf8(js, call->args[0], NULL) : NULL;
    if (!name) return call->argc ? (ui_js_val){0} : ui_js_undefined(js);
    const char *path;
    size_t len;
    uint32_t version;
    ui_js_val out = ui_js_undefined(js);
    if (parse_name(name, &path, &len, &version)) {
        dev_module *m = find(d, path, len);
        for (uint32_t i = 0; m && i < MAPS_KEPT; i++)
            if (m->maps[i].version == version && m->maps[i].json)
                out = ui_js_string(js, m->maps[i].json, strlen(m->maps[i].json));
    }
    free(name);
    return out;
}

static ui_js_val native_files(ui_js *js, const ui_js_call_info *call) {
    ui_dev *d = call->data;
    ui_js_val out = ui_js_array(js);
    uint32_t n = 0;
    for (uint32_t i = 0; i < d->module_count; i++) {
        if (d->modules[i].deleted) continue;
        ui_js_set_index(js, out, n++, ui_js_string(js, d->modules[i].path, strlen(d->modules[i].path)));
    }
    return out;
}

bool ui_dev_install(ui_dev *d, ui_js_val exports) {
    ui_js *js = d->js;
    static const struct {
        const char *name;
        uint32_t length;
        ui_js_native fn;
    } fns[] = {
        {"hotSetHandler", 1, native_set_handler}, {"hotBegin", 1, native_begin},
        {"hotCommit", 0, native_commit},          {"hotRevert", 0, native_revert},
        {"hotImporters", 1, native_importers},    {"hotSourceMap", 1, native_source_map},
        {"hotFiles", 0, native_files},
    };
    for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++) {
        ui_js_val f = ui_js_function(js, fns[i].name, fns[i].length, fns[i].fn, d);
        if (ui_js_is_empty(f) || !ui_js_set(js, exports, fns[i].name, f)) return false;
    }
    return ui_js_set(js, exports, "hotEntry", ui_js_string(js, d->ch->entry, strlen(d->ch->entry)));
}
