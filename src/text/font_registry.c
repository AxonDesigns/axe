#include "text/font_registry.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "text/fontconfig/fc.h"
#include "unicode/ucd.h"

#define MODULE "fonts"
#define MAX_AXES 3
#define FAMILY_MAX 96

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
    ui_font *font;
} font_entry;

typedef struct fallback_entry {
    uint32_t cp;
    uint32_t face; // UI_NO_FACE: discovery has nothing
} fallback_entry;

struct ui_font_registry {
    const ui_logger *log;
    ui_fc *fc; // NULL without system fonts or extra dirs
    face_entry *faces;
    uint32_t face_count, face_capacity;
    instance_entry *instances; // [0] is the default instance (no axes) of any face
    uint32_t instance_count, instance_capacity;
    font_entry *fonts;
    uint32_t font_count, font_capacity;
    fallback_entry *fallbacks;
    uint32_t fallback_count, fallback_capacity;
    // The cluster being resolved, read by match_covering through each_family.
    const uint32_t *query_cps;
    uint32_t query_n;
};

ui_result ui_font_registry_create(const ui_font_registry_desc *desc, ui_font_registry **out) {
    ui_font_registry *r = calloc(1, sizeof *r);
    if (!r) return UI_ERR_OUT_OF_MEMORY;
    r->log = desc->log;
    if (desc->system_fonts || desc->font_dir_count) {
        ui_result res = ui_fc_create(desc->system_fonts, desc->font_dirs, desc->font_dir_count,
                                     &r->fc);
        if (res != UI_OK) {
            UI_WARN(r->log, MODULE, "font discovery unavailable (%s)", ui_result_str(res));
            r->fc = NULL;
        }
    }
    if (ui_grow((void **)&r->instances, &r->instance_capacity, 1, sizeof *r->instances) != UI_OK) {
        ui_font_registry_destroy(r);
        return UI_ERR_OUT_OF_MEMORY;
    }
    r->instances[0] = (instance_entry){0};
    r->instance_count = 1;
    *out = r;
    return UI_OK;
}

void ui_font_registry_destroy(ui_font_registry *r) {
    if (!r) return;
    for (uint32_t i = 0; i < r->font_count; i++) ui_font_destroy(r->fonts[i].font);
    for (uint32_t i = 0; i < r->face_count; i++) {
        ui_face_destroy(r->faces[i].face);
        free(r->faces[i].path);
    }
    free(r->fonts);
    free(r->faces);
    free(r->instances);
    free(r->fallbacks);
    ui_fc_destroy(r->fc);
    free(r);
}

static ui_result add_face(ui_font_registry *r, const char *path, uint32_t index, uint32_t *id) {
    for (uint32_t i = 0; i < r->face_count; i++) {
        if (r->faces[i].path && r->faces[i].index == index && !strcmp(r->faces[i].path, path)) {
            *id = i;
            return UI_OK;
        }
    }
    if (ui_grow((void **)&r->faces, &r->face_capacity, r->face_count + 1, sizeof *r->faces) != UI_OK)
        return UI_ERR_OUT_OF_MEMORY;
    char *copy = strdup(path);
    if (!copy) return UI_ERR_OUT_OF_MEMORY;
    ui_face *face;
    ui_result res = ui_face_load_file(path, index, &face);
    if (res != UI_OK) {
        free(copy);
        return res;
    }
    r->faces[r->face_count] = (face_entry){face, copy, index};
    *id = r->face_count++;
    const ui_face_info *info = ui_face_get_info(face);
    UI_DEBUG(r->log, MODULE, "face %u: %s (weight %u%s%s) from %s", *id, info->family,
             info->weight, info->italic ? ", italic" : "", info->has_wght ? ", variable" : "",
             path);
    return UI_OK;
}

ui_result ui_font_registry_add_file(ui_font_registry *r, const char *path, uint32_t index,
                                    uint32_t *face_id) {
    uint32_t id;
    ui_result res = add_face(r, path, index, &id);
    if (res == UI_OK && face_id) *face_id = id;
    if (res != UI_OK) UI_WARN(r->log, MODULE, "cannot load %s: %s", path, ui_result_str(res));
    return res;
}

uint32_t ui_font_registry_face_count(const ui_font_registry *r) { return r->face_count; }

ui_face *ui_font_registry_face(const ui_font_registry *r, uint32_t id) {
    return id < r->face_count ? r->faces[id].face : NULL;
}

// ---- Matching ----------------------------------------------------------------------------

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

// The best registered face of `family`, or UI_NO_FACE.
static uint32_t best_of_family(const ui_font_registry *r, const char *family, uint16_t weight,
                               ui_font_style style) {
    uint32_t best = UI_NO_FACE;
    float best_score = INFINITY;
    for (uint32_t i = 0; i < r->face_count; i++) {
        const ui_face_info *info = ui_face_get_info(r->faces[i].face);
        if (strcasecmp(info->family, family)) continue;
        float s = face_score(info, weight, style);
        if (s < best_score) {
            best_score = s;
            best = i;
        }
    }
    return best;
}

static uint32_t discover(ui_font_registry *r, const char *family, uint16_t weight,
                         ui_font_style style) {
    if (!r->fc) return UI_NO_FACE;
    ui_fc_font found;
    if (!ui_fc_match(r->fc, family, weight, wants_slant(style), &found)) return UI_NO_FACE;
    uint32_t id;
    if (add_face(r, found.path, found.index, &id) != UI_OK) return UI_NO_FACE;
    // Pick the best of the now-registered family (the discovered face may be one of many).
    uint32_t best = best_of_family(r, ui_face_get_info(r->faces[id].face)->family, weight, style);
    return best != UI_NO_FACE ? best : id;
}

static bool is_generic(const char *f) {
    return !strcasecmp(f, "sans-serif") || !strcasecmp(f, "serif") ||
           !strcasecmp(f, "monospace") || !strcasecmp(f, "system-ui");
}

// Splits a CSS family list. Calls `fn` with each trimmed, unquoted name until it returns
// a face.
static uint32_t each_family(ui_font_registry *r, const char *list, uint32_t len,
                            uint32_t (*fn)(ui_font_registry *, const char *, uint16_t,
                                           ui_font_style),
                            uint16_t weight, ui_font_style style) {
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
        uint32_t face = fn(r, name, weight, style);
        if (face != UI_NO_FACE) return face;
    }
    return UI_NO_FACE;
}

static uint32_t match_one(ui_font_registry *r, const char *family, uint16_t weight,
                          ui_font_style style) {
    if (!is_generic(family)) {
        uint32_t face = best_of_family(r, family, weight, style);
        if (face != UI_NO_FACE) return face;
    }
    return discover(r, family, weight, style);
}

static uint32_t default_face(ui_font_registry *r, uint16_t weight, ui_font_style style) {
    uint32_t face = discover(r, "sans-serif", weight, style);
    if (face != UI_NO_FACE) return face;
    if (!r->face_count) return UI_NO_FACE;
    return best_of_family(r, ui_face_get_info(r->faces[0].face)->family, weight, style);
}

uint32_t ui_font_registry_match(ui_font_registry *r, const char *families, uint32_t len,
                                uint16_t weight, ui_font_style style) {
    uint32_t face = each_family(r, families, len, match_one, weight, style);
    return face != UI_NO_FACE ? face : default_face(r, weight, style);
}

// ---- Fallback ----------------------------------------------------------------------------

bool ui_font_registry_covers(const ui_font_registry *r, uint32_t face, const uint32_t *cps,
                             uint32_t n) {
    if (face >= r->face_count) return false;
    const ui_face *f = r->faces[face].face;
    for (uint32_t i = 0; i < n; i++) {
        if (ui_ucd(cps[i])->flags & UI_UCD_DEFAULT_IGNORABLE) continue;
        if (!ui_face_has_codepoint(f, cps[i])) return false;
    }
    return true;
}

static uint32_t match_covering(ui_font_registry *r, const char *family, uint16_t weight,
                               ui_font_style style) {
    uint32_t face = match_one(r, family, weight, style);
    return face != UI_NO_FACE && ui_font_registry_covers(r, face, r->query_cps, r->query_n) ? face : UI_NO_FACE;
}

static uint32_t discover_fallback(ui_font_registry *r, const uint32_t *cps, uint32_t n,
                                  uint16_t weight, ui_font_style style) {
    if (!r->fc) return UI_NO_FACE;
    uint32_t key = 0;
    for (uint32_t i = 0; i < n && !key; i++) {
        if (!(ui_ucd(cps[i])->flags & UI_UCD_DEFAULT_IGNORABLE)) key = cps[i];
    }
    for (uint32_t i = 0; i < r->fallback_count; i++) {
        if (r->fallbacks[i].cp != key) continue;
        uint32_t face = r->fallbacks[i].face;
        if (face == UI_NO_FACE || ui_font_registry_covers(r, face, cps, n)) return face;
        break; // the cached face covers the key but not the whole cluster: ask again
    }
    ui_fc_font found;
    uint32_t face = UI_NO_FACE;
    if (ui_fc_fallback(r->fc, cps, n, weight, wants_slant(style), &found))
        add_face(r, found.path, found.index, &face);
    if (ui_grow((void **)&r->fallbacks, &r->fallback_capacity, r->fallback_count + 1,
                sizeof *r->fallbacks) == UI_OK)
        r->fallbacks[r->fallback_count++] = (fallback_entry){key, face};
    return face;
}

uint32_t ui_font_registry_resolve(ui_font_registry *r, uint32_t primary, const char *families,
                                  uint32_t len, uint16_t weight, ui_font_style style,
                                  const uint32_t *cps, uint32_t n) {
    if (primary == UI_NO_FACE || ui_font_registry_covers(r, primary, cps, n)) return primary;
    r->query_cps = cps;
    r->query_n = n;
    uint32_t face = each_family(r, families, len, match_covering, weight, style);
    if (face != UI_NO_FACE) return face;
    // Registered families in registration order, each at its best weight and style.
    for (uint32_t i = 0; i < r->face_count; i++) {
        const char *family = ui_face_get_info(r->faces[i].face)->family;
        bool seen = false;
        for (uint32_t j = 0; j < i && !seen; j++)
            seen = !strcasecmp(ui_face_get_info(r->faces[j].face)->family, family);
        if (seen) continue;
        uint32_t best = best_of_family(r, family, weight, style);
        if (best != UI_NO_FACE && ui_font_registry_covers(r, best, cps, n)) return best;
    }
    face = discover_fallback(r, cps, n, weight, style);
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

static ui_font *find_font(ui_font_registry *r, ui_font_key key) {
    for (uint32_t i = 0; i < r->font_count; i++) {
        ui_font_key k = r->fonts[i].key;
        if (k.face == key.face && k.instance == key.instance && k.size_q == key.size_q)
            return r->fonts[i].font;
    }
    return NULL;
}

static ui_font *create_font(ui_font_registry *r, ui_font_key key) {
    if (key.face >= r->face_count || key.instance >= r->instance_count) return NULL;
    if (ui_grow((void **)&r->fonts, &r->font_capacity, r->font_count + 1, sizeof *r->fonts) != UI_OK)
        return NULL;
    const instance_entry *inst = &r->instances[key.instance];
    ui_font *font;
    if (ui_font_create(r->faces[key.face].face, (float)key.size_q / 64.f, inst->axes,
                       inst->axis_count, &inst->synth, &font) != UI_OK)
        return NULL;
    r->fonts[r->font_count++] = (font_entry){key, font};
    return font;
}

ui_font *ui_font_registry_font(ui_font_registry *r, uint32_t face, float size_px,
                               uint16_t weight, ui_font_style style, ui_font_key *key_out) {
    if (face >= r->face_count) return NULL;
    const ui_face_info *info = ui_face_get_info(r->faces[face].face);
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
    uint32_t instance = intern_instance(r, face, axes, count, synth);
    if (instance == UINT32_MAX) return NULL;
    uint32_t size_q = (uint32_t)lroundf(fminf(fmaxf(size_px, 1.f / 64.f), 4096.f) * 64.f);
    ui_font_key key = {face, instance, size_q};
    if (key_out) *key_out = key;
    ui_font *font = find_font(r, key);
    return font ? font : create_font(r, key);
}

ui_font *ui_font_registry_font_by_key(ui_font_registry *r, ui_font_key key) {
    ui_font *font = find_font(r, key);
    return font ? font : create_font(r, key);
}
