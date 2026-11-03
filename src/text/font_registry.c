#include "text/font_registry.h"

#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <threads.h>

#include "text/fontconfig/fc.h"
#include "unicode/ucd.h"

#define MODULE "fonts"
#define MAX_AXES 3
#define FAMILY_MAX 96
#define FACE_CHUNK 64u
#define FACE_CHUNKS 256u // at most 16,384 faces

typedef struct face_entry {
    ui_face *face;
    char *path;          // owned; NULL for faces not loaded from a file
    uint32_t index;
} face_entry;

typedef struct instance_entry {
    uint32_t face;
    uint32_t axis_count;
    ui_font_axis axes[MAX_AXES];
    ui_font_synthesis synth;
} instance_entry;

typedef struct font_entry {
    ui_font_key key;
    ui_font *font;       // NULL: empty slot
} font_entry;

typedef struct fallback_entry {
    uint32_t cp;
    uint32_t face; // UI_NO_FACE: discovery has nothing
} fallback_entry;

typedef struct discovery_entry {
    char family[FAMILY_MAX];
    uint16_t weight;
    uint8_t style;
    uint32_t face;       // UI_NO_FACE: discovery has no such family
} discovery_entry;

enum { DISCOVERY_OFF, DISCOVERY_PENDING, DISCOVERY_READY };

struct ui_font_registry {
    const ui_logger *log;
    // Face table: append-only chunks. An entry is written before face_count is published
    // (release), so a reader that sees the count (acquire) needs no lock.
    face_entry *chunks[FACE_CHUNKS];
    _Atomic uint32_t face_count;
    _Atomic uint64_t generation;
    _Atomic int discovery;
    _Atomic(ui_fc *) fc;  // set once discovery is ready
    bool system_fonts;
    char **font_dirs;
    uint32_t font_dir_count;

    mtx_t fc_lock;        // Fontconfig queries, made without holding `lock`
    mtx_t lock;           // everything below, and appending faces
    uint32_t *registered; // face ids in registration order
    uint32_t registered_count, registered_capacity;
    instance_entry *instances; // [0] is the default instance (no axes) of any face
    uint32_t instance_count, instance_capacity;
    font_entry *fonts;    // open addressing, power-of-two capacity
    uint32_t font_count, font_capacity;
    fallback_entry *fallbacks;
    uint32_t fallback_count, fallback_capacity;
    discovery_entry *discoveries;
    uint32_t discovery_count, discovery_capacity;
};

// What a lookup is for: matching a family list, or covering one cluster.
typedef struct query {
    uint16_t weight;
    ui_font_style style;
    const uint32_t *cps;  // the cluster being resolved (match_covering only)
    uint32_t n;
} query;

static face_entry *entry_of(const ui_font_registry *r, uint32_t id) {
    return &r->chunks[id / FACE_CHUNK][id % FACE_CHUNK];
}

static uint32_t face_count(const ui_font_registry *r) {
    return atomic_load_explicit(&r->face_count, memory_order_acquire);
}

ui_result ui_font_registry_create(const ui_font_registry_desc *desc, ui_font_registry **out) {
    ui_font_registry *r = calloc(1, sizeof *r);
    if (!r) return UI_ERR_OUT_OF_MEMORY;
    r->log = desc->log;
    if (mtx_init(&r->lock, mtx_plain) != thrd_success) {
        free(r);
        return UI_ERR_OUT_OF_MEMORY;
    }
    if (mtx_init(&r->fc_lock, mtx_plain) != thrd_success) {
        mtx_destroy(&r->lock);
        free(r);
        return UI_ERR_OUT_OF_MEMORY;
    }
    atomic_init(&r->face_count, 0u);
    atomic_init(&r->generation, 0u);
    atomic_init(&r->fc, (ui_fc *)NULL);
    atomic_init(&r->discovery, DISCOVERY_OFF);
    r->system_fonts = desc->system_fonts;
    if (desc->font_dir_count) {
        r->font_dirs = calloc(desc->font_dir_count, sizeof *r->font_dirs);
        if (!r->font_dirs) goto oom;
        for (uint32_t i = 0; i < desc->font_dir_count; i++) {
            r->font_dirs[i] = strdup(desc->font_dirs[i]);
            if (!r->font_dirs[i]) goto oom;
            r->font_dir_count++;
        }
    }
    if (ui_grow((void **)&r->instances, &r->instance_capacity, 1, sizeof *r->instances) != UI_OK)
        goto oom;
    r->instances[0] = (instance_entry){0};
    r->instance_count = 1;
    if (desc->system_fonts || desc->font_dir_count) {
        atomic_store(&r->discovery, DISCOVERY_PENDING);
        if (!desc->defer_discovery) ui_font_registry_start_discovery(r);
    }
    *out = r;
    return UI_OK;
oom:
    ui_font_registry_destroy(r);
    return UI_ERR_OUT_OF_MEMORY;
}

void ui_font_registry_start_discovery(ui_font_registry *r) {
    if (atomic_load(&r->discovery) != DISCOVERY_PENDING) return;
    ui_fc *fc = NULL;
    ui_result res = ui_fc_create(r->system_fonts, (const char *const *)r->font_dirs,
                                 r->font_dir_count, &fc);
    if (res != UI_OK) {
        UI_WARN(r->log, MODULE, "font discovery unavailable (%s)", ui_result_str(res));
        fc = NULL;
    }
    atomic_store_explicit(&r->fc, fc, memory_order_release);
    atomic_store_explicit(&r->discovery, fc ? DISCOVERY_READY : DISCOVERY_OFF,
                          memory_order_release);
}

bool ui_font_registry_discovery_pending(const ui_font_registry *r) {
    return atomic_load_explicit(&r->discovery, memory_order_acquire) == DISCOVERY_PENDING;
}

void ui_font_registry_destroy(ui_font_registry *r) {
    if (!r) return;
    for (uint32_t i = 0; i < r->font_capacity; i++) {
        if (r->fonts[i].font) ui_font_destroy(r->fonts[i].font);
    }
    uint32_t n = face_count(r);
    for (uint32_t i = 0; i < n; i++) {
        ui_face_destroy(entry_of(r, i)->face);
        free(entry_of(r, i)->path);
    }
    for (uint32_t i = 0; i < FACE_CHUNKS; i++) free(r->chunks[i]);
    for (uint32_t i = 0; i < r->font_dir_count; i++) free(r->font_dirs[i]);
    free(r->font_dirs);
    free(r->fonts);
    free(r->registered);
    free(r->instances);
    free(r->fallbacks);
    free(r->discoveries);
    ui_fc_destroy(atomic_load(&r->fc));
    mtx_destroy(&r->fc_lock);
    mtx_destroy(&r->lock);
    free(r);
}

// The face already loaded from (path, index), or UI_NO_FACE. Lock held.
static uint32_t find_path(const ui_font_registry *r, const char *path, uint32_t index) {
    uint32_t n = face_count(r);
    for (uint32_t i = 0; i < n; i++) {
        const face_entry *e = entry_of(r, i);
        if (e->path && e->index == index && !strcmp(e->path, path)) return i;
    }
    return UI_NO_FACE;
}

// Appends a loaded face (taking ownership), or returns the one already loaded from the same
// file and destroys `face`. Lock held.
static uint32_t insert_face(ui_font_registry *r, const char *path, uint32_t index,
                            ui_face *face) {
    uint32_t id = find_path(r, path, index);
    if (id != UI_NO_FACE) {
        ui_face_destroy(face);
        return id;
    }
    id = face_count(r);
    uint32_t chunk = id / FACE_CHUNK;
    if (chunk >= FACE_CHUNKS) goto fail;
    if (!r->chunks[chunk] && !(r->chunks[chunk] = calloc(FACE_CHUNK, sizeof(face_entry))))
        goto fail;
    char *copy = strdup(path);
    if (!copy) goto fail;
    *entry_of(r, id) = (face_entry){face, copy, index};
    atomic_store_explicit(&r->face_count, id + 1, memory_order_release);
    const ui_face_info *info = ui_face_get_info(face);
    UI_DEBUG(r->log, MODULE, "face %u: %s (weight %u%s%s) from %s", id, info->family,
             info->weight, info->italic ? ", italic" : "", info->has_wght ? ", variable" : "",
             path);
    return id;
fail:
    ui_face_destroy(face);
    return UI_NO_FACE;
}

// The face for a file, loading it without holding the lock (the lock is held on entry and
// on return, but released while reading the file). UI_NO_FACE if it can't be loaded.
static uint32_t face_for_path(ui_font_registry *r, const char *path, uint32_t index) {
    uint32_t id = find_path(r, path, index);
    if (id != UI_NO_FACE) return id;
    mtx_unlock(&r->lock);
    ui_face *face = NULL;
    ui_result res = ui_face_load_file(path, index, &face);
    mtx_lock(&r->lock);
    if (res != UI_OK) return UI_NO_FACE;
    return insert_face(r, path, index, face);
}

static bool is_registered(const ui_font_registry *r, uint32_t id) {
    for (uint32_t i = 0; i < r->registered_count; i++) {
        if (r->registered[i] == id) return true;
    }
    return false;
}

ui_result ui_font_registry_register(ui_font_registry *r, const char *path, uint32_t index,
                                    ui_face *face, uint32_t *face_id, bool *changed) {
    if (changed) *changed = false;
    mtx_lock(&r->lock);
    uint32_t id = insert_face(r, path, index, face);
    ui_result res = UI_OK;
    if (id == UI_NO_FACE) {
        res = UI_ERR_OUT_OF_MEMORY;
    } else if (!is_registered(r, id)) {
        if (ui_grow((void **)&r->registered, &r->registered_capacity, r->registered_count + 1,
                    sizeof *r->registered) != UI_OK) {
            res = UI_ERR_OUT_OF_MEMORY;
        } else {
            r->registered[r->registered_count++] = id;
            atomic_fetch_add_explicit(&r->generation, 1u, memory_order_release);
            if (changed) *changed = true;
        }
    }
    mtx_unlock(&r->lock);
    if (res == UI_OK && face_id) *face_id = id;
    return res;
}

ui_result ui_font_registry_add_file(ui_font_registry *r, const char *path, uint32_t index,
                                    uint32_t *face_id) {
    ui_face *face;
    ui_result res = ui_face_load_file(path, index, &face);
    if (res == UI_OK) res = ui_font_registry_register(r, path, index, face, face_id, NULL);
    if (res != UI_OK) UI_WARN(r->log, MODULE, "cannot load %s: %s", path, ui_result_str(res));
    return res;
}

uint64_t ui_font_registry_generation(const ui_font_registry *r) {
    return atomic_load_explicit(&r->generation, memory_order_acquire);
}

uint32_t ui_font_registry_face_count(const ui_font_registry *r) { return face_count(r); }

uint32_t ui_font_registry_registered_count(ui_font_registry *r) {
    mtx_lock(&r->lock);
    uint32_t n = r->registered_count;
    mtx_unlock(&r->lock);
    return n;
}

ui_face *ui_font_registry_face(const ui_font_registry *r, uint32_t id) {
    return id < face_count(r) ? entry_of(r, id)->face : NULL;
}

// ---- Matching ----------------------------------------------------------------------------
// Everything below runs with `lock` held unless it says otherwise.

static bool wants_slant(ui_font_style style) { return style != UI_FONT_NORMAL; }

// CSS Fonts 4 §5.2 weight matching expressed as a distance; lower is better.
static float weight_distance(const ui_face_info *info, uint16_t want) {
    float w = want;
    float have = info->has_wght ? fminf(fmaxf(w, info->wght_min), info->wght_max) : info->weight;
    if (w >= 400 && w <= 500) {
        if (have >= w && have <= 500) return have - w;
        if (have < w) return (w - have) + 1000;
        return (have - w) + 2000;
    }
    if (w < 400) return have <= w ? w - have : (have - w) + 1000;
    return have >= w ? have - w : (w - have) + 1000;
}

static float face_score(const ui_face_info *info, uint16_t weight, ui_font_style style) {
    bool slanted_capable = info->italic || info->has_ital || info->has_slnt;
    float penalty = 0;
    if (wants_slant(style) && !slanted_capable) penalty = 10000;
    if (!wants_slant(style) && info->italic) penalty = 10000;
    return penalty + weight_distance(info, weight);
}

// The best face of `family` among the registered faces, or among every face known (also
// those found by discovery). UI_NO_FACE if there is none.
static uint32_t best_of_family(const ui_font_registry *r, const char *family, bool registered_only,
                               uint16_t weight, ui_font_style style) {
    uint32_t best = UI_NO_FACE;
    float best_score = INFINITY;
    uint32_t n = registered_only ? r->registered_count : face_count(r);
    for (uint32_t k = 0; k < n; k++) {
        uint32_t i = registered_only ? r->registered[k] : k;
        const ui_face_info *info = ui_face_get_info(entry_of(r, i)->face);
        if (strcasecmp(info->family, family)) continue;
        float s = face_score(info, weight, style);
        if (s < best_score || (s == best_score && i < best)) {
            best_score = s;
            best = i;
        }
    }
    return best;
}

static ui_fc *discovery_of(const ui_font_registry *r) {
    return atomic_load_explicit(&r->fc, memory_order_acquire);
}

static uint32_t discover(ui_font_registry *r, const char *family, const query *q) {
    ui_fc *fc = discovery_of(r);
    if (!fc) return UI_NO_FACE;
    for (uint32_t i = 0; i < r->discovery_count; i++) {
        const discovery_entry *d = &r->discoveries[i];
        if (d->weight == q->weight && d->style == q->style && !strcasecmp(d->family, family))
            return d->face;
    }
    char name[FAMILY_MAX];
    snprintf(name, sizeof name, "%s", family);
    mtx_unlock(&r->lock);
    ui_fc_font found;
    mtx_lock(&r->fc_lock);
    bool ok = ui_fc_match(fc, name, q->weight, wants_slant(q->style), &found);
    mtx_unlock(&r->fc_lock);
    mtx_lock(&r->lock);
    uint32_t face = ok ? face_for_path(r, found.path, found.index) : UI_NO_FACE;
    if (face != UI_NO_FACE) {
        // Pick the best of that family among every face known (the discovered face may be
        // one of many).
        uint32_t best = best_of_family(r, ui_face_get_info(entry_of(r, face)->face)->family,
                                       false, q->weight, q->style);
        if (best != UI_NO_FACE) face = best;
    }
    if (ui_grow((void **)&r->discoveries, &r->discovery_capacity, r->discovery_count + 1,
                sizeof *r->discoveries) == UI_OK) {
        discovery_entry *d = &r->discoveries[r->discovery_count++];
        *d = (discovery_entry){.weight = q->weight, .style = (uint8_t)q->style, .face = face};
        snprintf(d->family, sizeof d->family, "%s", name);
    }
    return face;
}

static bool is_generic(const char *f) {
    return !strcasecmp(f, "sans-serif") || !strcasecmp(f, "serif") ||
           !strcasecmp(f, "monospace") || !strcasecmp(f, "system-ui");
}

// Splits a CSS family list. Calls `fn` with each trimmed, unquoted name until it returns
// a face.
static uint32_t each_family(ui_font_registry *r, const char *list, uint32_t len,
                            uint32_t (*fn)(ui_font_registry *, const char *, const query *),
                            const query *q) {
    uint32_t i = 0;
    while (list && i < len) {
        while (i < len && (isspace((unsigned char)list[i]) || list[i] == ',')) i++;
        if (i >= len) break;
        char name[FAMILY_MAX];
        uint32_t n = 0;
        char quote = 0;
        if (list[i] == '"' || list[i] == '\'') quote = list[i++];
        while (i < len && (quote ? list[i] != quote : list[i] != ',')) {
            if (n + 1 < sizeof name) name[n++] = list[i];
            i++;
        }
        if (quote && i < len) i++;
        while (n && isspace((unsigned char)name[n - 1])) n--;
        name[n] = 0;
        if (!n) continue;
        uint32_t face = fn(r, name, q);
        if (face != UI_NO_FACE) return face;
    }
    return UI_NO_FACE;
}

static uint32_t match_one(ui_font_registry *r, const char *family, const query *q) {
    if (!is_generic(family)) {
        uint32_t face = best_of_family(r, family, true, q->weight, q->style);
        if (face != UI_NO_FACE) return face;
    }
    return discover(r, family, q);
}

static uint32_t default_face(ui_font_registry *r, const query *q) {
    uint32_t face = discover(r, "sans-serif", q);
    if (face != UI_NO_FACE) return face;
    if (!r->registered_count) return UI_NO_FACE;
    const char *family = ui_face_get_info(entry_of(r, r->registered[0])->face)->family;
    return best_of_family(r, family, true, q->weight, q->style);
}

uint32_t ui_font_registry_match(ui_font_registry *r, const char *families, uint32_t len,
                                uint16_t weight, ui_font_style style) {
    query q = {weight, style, NULL, 0};
    mtx_lock(&r->lock);
    uint32_t face = each_family(r, families, len, match_one, &q);
    if (face == UI_NO_FACE) face = default_face(r, &q);
    mtx_unlock(&r->lock);
    return face;
}

// ---- Fallback ----------------------------------------------------------------------------

// Takes no lock: the face table is published.
bool ui_font_registry_covers(const ui_font_registry *r, uint32_t face, const uint32_t *cps,
                             uint32_t n) {
    if (face >= face_count(r)) return false;
    const ui_face *f = entry_of(r, face)->face;
    for (uint32_t i = 0; i < n; i++) {
        if (ui_ucd(cps[i])->flags & UI_UCD_DEFAULT_IGNORABLE) continue;
        if (!ui_face_has_codepoint(f, cps[i])) return false;
    }
    return true;
}

static uint32_t match_covering(ui_font_registry *r, const char *family, const query *q) {
    uint32_t face = match_one(r, family, q);
    return face != UI_NO_FACE && ui_font_registry_covers(r, face, q->cps, q->n) ? face : UI_NO_FACE;
}

static uint32_t discover_fallback(ui_font_registry *r, const query *q) {
    ui_fc *fc = discovery_of(r);
    if (!fc) return UI_NO_FACE;
    uint32_t key = 0;
    for (uint32_t i = 0; i < q->n && !key; i++) {
        if (!(ui_ucd(q->cps[i])->flags & UI_UCD_DEFAULT_IGNORABLE)) key = q->cps[i];
    }
    for (uint32_t i = 0; i < r->fallback_count; i++) {
        if (r->fallbacks[i].cp != key) continue;
        uint32_t face = r->fallbacks[i].face;
        if (face == UI_NO_FACE || ui_font_registry_covers(r, face, q->cps, q->n)) return face;
        break; // the cached face covers the key but not the whole cluster: ask again
    }
    mtx_unlock(&r->lock);
    ui_fc_font found;
    mtx_lock(&r->fc_lock);
    bool ok = ui_fc_fallback(fc, q->cps, q->n, q->weight, wants_slant(q->style), &found);
    mtx_unlock(&r->fc_lock);
    mtx_lock(&r->lock);
    uint32_t face = ok ? face_for_path(r, found.path, found.index) : UI_NO_FACE;
    if (ui_grow((void **)&r->fallbacks, &r->fallback_capacity, r->fallback_count + 1,
                sizeof *r->fallbacks) == UI_OK)
        r->fallbacks[r->fallback_count++] = (fallback_entry){key, face};
    return face;
}

uint32_t ui_font_registry_resolve(ui_font_registry *r, uint32_t primary, const char *families,
                                  uint32_t len, uint16_t weight, ui_font_style style,
                                  const uint32_t *cps, uint32_t n) {
    if (primary == UI_NO_FACE || ui_font_registry_covers(r, primary, cps, n)) return primary;
    query q = {weight, style, cps, n};
    mtx_lock(&r->lock);
    uint32_t face = each_family(r, families, len, match_covering, &q);
    // Registered families in registration order, each at its best weight and style.
    for (uint32_t i = 0; face == UI_NO_FACE && i < r->registered_count; i++) {
        const char *family = ui_face_get_info(entry_of(r, r->registered[i])->face)->family;
        bool seen = false;
        for (uint32_t j = 0; j < i && !seen; j++)
            seen = !strcasecmp(ui_face_get_info(entry_of(r, r->registered[j])->face)->family,
                               family);
        if (seen) continue;
        uint32_t best = best_of_family(r, family, true, weight, style);
        if (best != UI_NO_FACE && ui_font_registry_covers(r, best, cps, n)) face = best;
    }
    if (face == UI_NO_FACE) face = discover_fallback(r, &q);
    mtx_unlock(&r->lock);
    return face != UI_NO_FACE ? face : primary;
}

// ---- Instances ---------------------------------------------------------------------------

static uint32_t intern_instance(ui_font_registry *r, uint32_t face, const ui_font_axis *axes,
                                uint32_t count, ui_font_synthesis synth) {
    if (!count && synth.embolden == 0 && synth.slant == 0) return 0;
    for (uint32_t i = 1; i < r->instance_count; i++) {
        instance_entry *e = &r->instances[i];
        if (e->face == face && e->axis_count == count &&
            !memcmp(e->axes, axes, count * sizeof *axes) &&
            e->synth.embolden == synth.embolden && e->synth.slant == synth.slant)
            return i;
    }
    if (ui_grow((void **)&r->instances, &r->instance_capacity, r->instance_count + 1,
                sizeof *r->instances) != UI_OK)
        return UINT32_MAX;
    instance_entry *e = &r->instances[r->instance_count];
    *e = (instance_entry){.face = face, .axis_count = count, .synth = synth};
    memcpy(e->axes, axes, count * sizeof *axes);
    return r->instance_count++;
}

static uint32_t key_hash(ui_font_key k) {
    uint32_t h = k.face * 0x9E3779B1u ^ k.instance * 0x85EBCA77u ^ k.size_q * 0xC2B2AE3Du;
    return h ^ (h >> 15);
}

static bool same_key(ui_font_key a, ui_font_key b) {
    return a.face == b.face && a.instance == b.instance && a.size_q == b.size_q;
}

static font_entry *font_slot(const ui_font_registry *r, ui_font_key key) {
    uint32_t mask = r->font_capacity - 1;
    for (uint32_t i = key_hash(key) & mask;; i = (i + 1) & mask) {
        font_entry *e = &r->fonts[i];
        if (!e->font || same_key(e->key, key)) return e;
    }
}

static ui_font *find_font(const ui_font_registry *r, ui_font_key key) {
    return r->font_capacity ? font_slot(r, key)->font : NULL;
}

static bool grow_fonts(ui_font_registry *r) {
    uint32_t cap = r->font_capacity ? r->font_capacity * 2 : 64;
    font_entry *old = r->fonts;
    uint32_t old_cap = r->font_capacity;
    r->fonts = calloc(cap, sizeof *r->fonts);
    if (!r->fonts) {
        r->fonts = old;
        return false;
    }
    r->font_capacity = cap;
    for (uint32_t i = 0; i < old_cap; i++) {
        if (old[i].font) *font_slot(r, old[i].key) = old[i];
    }
    free(old);
    return true;
}

static ui_font *create_font(ui_font_registry *r, ui_font_key key) {
    if (key.face >= face_count(r) || key.instance >= r->instance_count) return NULL;
    if ((r->font_count + 1) * 2 > r->font_capacity && !grow_fonts(r)) return NULL;
    const instance_entry *inst = &r->instances[key.instance];
    ui_font *font;
    if (ui_font_create(entry_of(r, key.face)->face, (float)key.size_q / 64.f, inst->axes,
                       inst->axis_count, &inst->synth, &font) != UI_OK)
        return NULL;
    *font_slot(r, key) = (font_entry){key, font};
    r->font_count++;
    return font;
}

ui_font *ui_font_registry_font(ui_font_registry *r, uint32_t face, float size_px,
                               uint16_t weight, ui_font_style style, ui_font_key *key_out) {
    if (face >= face_count(r)) return NULL;
    const ui_face_info *info = ui_face_get_info(entry_of(r, face)->face);
    ui_font_axis axes[MAX_AXES];
    uint32_t count = 0;
    if (info->has_wght) {
        float w = fminf(fmaxf((float)weight, info->wght_min), info->wght_max);
        axes[count++] = (ui_font_axis){UI_AXIS_WGHT, w};
    }
    if (wants_slant(style) && info->has_ital && style == UI_FONT_ITALIC)
        axes[count++] = (ui_font_axis){UI_AXIS_ITAL, 1};
    else if (wants_slant(style) && info->has_slnt)
        axes[count++] = (ui_font_axis){UI_AXIS_SLNT, fmaxf(info->slnt_min, -12.f)};
    // CSS font-synthesis: what the face can't provide is synthesized. Bold when a bold
    // weight is asked of a face that only reaches a regular one; a slant when italic or
    // oblique is asked of a face with no italic design or axis.
    ui_font_synthesis synth = {0};
    float reach = info->has_wght ? fminf(fmaxf((float)weight, info->wght_min), info->wght_max)
                                 : (float)info->weight;
    if (weight >= 600 && reach <= 500) synth.embolden = UI_SYNTHETIC_EMBOLDEN;
    if (wants_slant(style) && !info->italic && !info->has_ital && !info->has_slnt)
        synth.slant = UI_SYNTHETIC_SLANT;
    uint32_t size_q = (uint32_t)lroundf(fminf(fmaxf(size_px, 1.f / 64.f), 4096.f) * 64.f);
    mtx_lock(&r->lock);
    uint32_t instance = intern_instance(r, face, axes, count, synth);
    ui_font *font = NULL;
    if (instance != UINT32_MAX) {
        ui_font_key key = {face, instance, size_q};
        if (key_out) *key_out = key;
        font = find_font(r, key);
        if (!font) font = create_font(r, key);
    }
    mtx_unlock(&r->lock);
    return font;
}

ui_font *ui_font_registry_font_by_key(ui_font_registry *r, ui_font_key key) {
    mtx_lock(&r->lock);
    ui_font *font = find_font(r, key);
    if (!font) font = create_font(r, key);
    mtx_unlock(&r->lock);
    return font;
}
