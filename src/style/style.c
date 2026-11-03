#include "style/style.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "core/color.h"
#include "style/props.h"
#include "wire/wire.h"

#define STYLE_BLOCK_HEADER 8u

// A removed slot in the string table.
static ui_istr string_tombstone;
#define STRING_TOMBSTONE(s) ((void)(s), &string_tombstone)

void ui_styles_init(ui_styles *s) { memset(s, 0, sizeof *s); }

static void free_def(ui_styles *s, ui_style_def *d) {
    for (uint32_t i = 0; i < d->entry_count; i++) ui_styles_release_value(s, &d->entries[i].value);
    free(d->blocks);
    free(d->entries);
    free(d->tokens);
}

void ui_styles_free(ui_styles *s) {
    for (uint32_t i = 0; i < s->def_capacity; i++) {
        if (s->defs[i].id) free_def(s, &s->defs[i]);
    }
    for (uint32_t i = 0; i < s->string_capacity; i++) {
        if (s->strings[i] && s->strings[i] != STRING_TOMBSTONE(s)) free(s->strings[i]);
    }
    free(s->defs);
    free(s->tokens);
    free(s->changed_styles);
    free(s->changed_tokens);
    free(s->strings);
    free(s->garbage);
    memset(s, 0, sizeof *s);
}

static uint32_t hash_id(uint32_t id) {
    id *= 0x9E3779B1u;
    return id ^ (id >> 16);
}

// ---- Interned strings -----------------------------------------------------------------------


static uint32_t hash_bytes(const char *p, uint32_t n) {
    uint32_t h = 2166136261u; // FNV-1a
    for (uint32_t i = 0; i < n; i++) h = (h ^ (uint8_t)p[i]) * 16777619u;
    return h;
}

static ui_istr **string_slot(const ui_styles *s, const char *p, uint32_t n, uint32_t h, bool *found) {
    uint32_t mask = s->string_capacity - 1;
    ui_istr **reuse = NULL;
    for (uint32_t i = h & mask;; i = (i + 1) & mask) {
        ui_istr **slot = &s->strings[i];
        if (!*slot) {
            *found = false;
            return reuse ? reuse : slot;
        }
        if (*slot == STRING_TOMBSTONE(s)) {
            if (!reuse) reuse = slot;
        } else if ((*slot)->hash == h && (*slot)->len == n && !memcmp((*slot)->bytes, p, n)) {
            *found = true;
            return slot;
        }
    }
}

static bool grow_strings(ui_styles *s) {
    if ((s->string_live + s->string_tombstones + 1) * 4 <= s->string_capacity * 3) return true;
    uint32_t cap = s->string_capacity ? s->string_capacity * 2 : 64;
    if (s->string_live * 4 < s->string_capacity) cap = s->string_capacity;
    ui_istr **old = s->strings;
    uint32_t old_cap = s->string_capacity;
    if (!(s->strings = calloc(cap, sizeof *s->strings))) {
        s->strings = old;
        return false;
    }
    s->string_capacity = cap;
    s->string_tombstones = 0;
    for (uint32_t i = 0; i < old_cap; i++) {
        if (!old[i] || old[i] == STRING_TOMBSTONE(s)) continue;
        bool found;
        *string_slot(s, old[i]->bytes, old[i]->len, old[i]->hash, &found) = old[i];
    }
    free(old);
    return true;
}

const char *ui_styles_intern(ui_styles *s, const char *bytes, uint32_t len) {
    if (!grow_strings(s)) return NULL;
    uint32_t h = hash_bytes(bytes, len);
    bool found;
    ui_istr **slot = string_slot(s, bytes, len, h, &found);
    if (found) {
        (*slot)->refs++;
        return (*slot)->bytes;
    }
    ui_istr *str = malloc(sizeof *str + len + 1);
    if (!str) return NULL;
    *str = (ui_istr){.refs = 1, .len = len, .hash = h};
    memcpy(str->bytes, bytes, len);
    str->bytes[len] = 0;
    if (*slot == STRING_TOMBSTONE(s)) s->string_tombstones--;
    *slot = str;
    s->string_live++;
    return str->bytes;
}

static ui_istr *mutable_istr(const char *bytes) { return (ui_istr *)(void *)(uintptr_t)ui_istr_of(bytes); }

void ui_styles_retain(ui_styles *s, const char *bytes) {
    (void)s;
    if (bytes) mutable_istr(bytes)->refs++;
}

void ui_styles_release(ui_styles *s, const char *bytes) {
    if (!bytes) return;
    ui_istr *str = mutable_istr(bytes);
    if (--str->refs || str->garbage) return;
    if (ui_grow((void **)&s->garbage, &s->garbage_capacity, s->garbage_count + 1, sizeof *s->garbage) != UI_OK)
        return; // out of memory: it stays interned (found again, never freed early)
    str->garbage = true;
    s->garbage[s->garbage_count++] = str;
}

void ui_styles_release_value(ui_styles *s, const ui_value *v) {
    if (v->kind == UI_VALUE_STRING) ui_styles_release(s, v->string.bytes);
}

void ui_styles_collect(ui_styles *s) {
    for (uint32_t i = 0; i < s->garbage_count; i++) {
        ui_istr *str = s->garbage[i];
        str->garbage = false;
        if (str->refs) continue; // interned again since
        bool found;
        ui_istr **slot = string_slot(s, str->bytes, str->len, str->hash, &found);
        *slot = STRING_TOMBSTONE(s);
        s->string_live--;
        s->string_tombstones++;
        free(str);
    }
    s->garbage_count = 0;
}

// ---- Framing and decoding ----------------------------------------------------------------

bool ui_style_walk(const uint8_t *p, uint32_t bytes, uint32_t count, uint32_t *end) {
    uint32_t off = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (bytes - off < UI_WIRE_STYLE_ENTRY_HEADER || off > bytes) return false;
        uint32_t words = p[off + 3];
        if (!words || words * 4u > bytes - off) return false;
        off += words * 4u;
    }
    *end = off;
    return true;
}

bool ui_style_walk_define(const uint8_t *payload, uint32_t size) {
    uint32_t header = ui_wire_define_style_header(payload);
    if (header < UI_WIRE_SIZE_DEFINE_STYLE_HEADER || header % 4 || header > size) return false;
    uint32_t blocks = ui_wire_get_u16(payload + 6), off = header;
    for (uint32_t b = 0; b < blocks; b++) {
        if (size - off < STYLE_BLOCK_HEADER) return false;
        const uint8_t *bp = payload + off;
        uint32_t bytes = ui_wire_get_u16(bp), bh = ui_wire_size_or(bp + 6, STYLE_BLOCK_HEADER);
        uint32_t end;
        if (bytes < STYLE_BLOCK_HEADER || bytes % 4 || bytes > size - off || bh < STYLE_BLOCK_HEADER ||
            bh % 4 || bh > bytes || !ui_style_walk(bp + bh, bytes - bh, ui_wire_get_u16(bp + 4), &end))
            return false;
        off += bytes;
    }
    return true;
}

// Reads the value of a known kind; false if the entry is too short for it.
static bool read_value(const uint8_t *e, ui_value *v) {
    uint8_t kind = e[2];
    uint32_t words = ui_style_kind_words(kind);
    if (e[3] < words) return false;
    const uint8_t *p = e + UI_WIRE_STYLE_ENTRY_HEADER;
    *v = (ui_value){.kind = (ui_value_kind)kind};
    switch (kind) {
    case UI_VALUE_PX:
    case UI_VALUE_PERCENT:
    case UI_VALUE_NUMBER: v->number = ui_wire_get_f32(p); break;
    case UI_VALUE_KEYWORD: v->keyword = ui_wire_get_u32(p); break;
    case UI_VALUE_TOKEN: v->token = ui_wire_get_u32(p); break;
    case UI_VALUE_SPRING:
        v->spring.stiffness = ui_wire_get_f32(p);
        v->spring.damping = ui_wire_get_f32(p + 4);
        break;
    case UI_VALUE_COLOR:
        for (int i = 0; i < 4; i++) v->color[i] = ui_wire_get_f32(p + 4 * i);
        break;
    case UI_VALUE_STRING: // the bytes stay in the buffer until interned
        v->string.length = ui_wire_get_u16(p);
        v->string.bytes = (const char *)p + 2;
        if (2 + v->string.length > (e[3] - 1u) * 4) return false;
        break;
    default: break;
    }
    return true;
}

// Interns a decoded string value in place (the buffer's bytes become the shared ones).
static bool intern_value(ui_styles *s, ui_value *v) {
    if (v->kind != UI_VALUE_STRING) return true;
    const char *b = ui_styles_intern(s, v->string.bytes, v->string.length);
    if (!b) return false;
    v->string.bytes = b;
    return true;
}

bool ui_style_decode(ui_styles *s, const uint8_t *p, uint32_t bytes, uint32_t count, bool override,
                     ui_style_entry *out, uint32_t *out_count) {
    (void)bytes; // framing checked by ui_style_walk
    uint32_t n = 0, off = 0;
    bool ok = true;
    for (uint32_t i = 0; ok && i < count; i++) {
        const uint8_t *e = p + off;
        off += e[3] * 4u;
        uint16_t prop = ui_wire_get_u16(e);
        // Newer writers' properties and kinds are skipped, not errors.
        if (!ui_style_prop_known(prop) || !ui_style_kind_words(e[2])) continue;
        ui_value v;
        ok = read_value(e, &v) && ui_style_value_valid(prop, &v, override) && intern_value(s, &v);
        if (ok) out[n++] = (ui_style_entry){prop, v};
    }
    if (!ok) {
        for (uint32_t i = 0; i < n; i++) ui_styles_release_value(s, &out[i].value);
        n = 0;
    }
    *out_count = n;
    return ok;
}

// ---- Styles ------------------------------------------------------------------------------

static ui_style_def *def_slot(const ui_styles *s, uint32_t id, bool *found) {
    uint32_t mask = s->def_capacity - 1;
    ui_style_def *reuse = NULL;
    for (uint32_t i = hash_id(id) & mask;; i = (i + 1) & mask) {
        ui_style_def *d = &s->defs[i];
        if (!d->id && !d->tombstone) {
            *found = false;
            return reuse ? reuse : d;
        }
        if (d->tombstone) {
            if (!reuse) reuse = d;
        } else if (d->id == id) {
            *found = true;
            return d;
        }
    }
}

static bool grow_defs(ui_styles *s) {
    if ((s->def_live + s->def_tombstones + 1) * 4 <= s->def_capacity * 3) return true;
    uint32_t cap = s->def_capacity ? s->def_capacity * 2 : 64;
    if (s->def_live * 4 < s->def_capacity) cap = s->def_capacity; // mostly tombstones
    ui_style_def *old = s->defs;
    uint32_t old_cap = s->def_capacity;
    s->defs = calloc(cap, sizeof *s->defs);
    if (!s->defs) {
        s->defs = old;
        return false;
    }
    s->def_capacity = cap;
    s->def_tombstones = 0;
    for (uint32_t i = 0; i < old_cap; i++) {
        bool found;
        if (old[i].id) *def_slot(s, old[i].id, &found) = old[i];
    }
    free(old);
    return true;
}

const ui_style_def *ui_styles_find(const ui_styles *s, uint32_t id) {
    if (!s->def_capacity || !id) return NULL;
    bool found;
    ui_style_def *d = def_slot(s, id, &found);
    return found ? d : NULL;
}

static bool note_change(uint32_t **list, uint32_t *count, uint32_t *capacity, uint32_t id) {
    for (uint32_t i = 0; i < *count; i++) {
        if ((*list)[i] == id) return true;
    }
    if (ui_grow((void **)list, capacity, *count + 1, sizeof **list) != UI_OK) return false;
    (*list)[(*count)++] = id;
    return true;
}

bool ui_styles_define(ui_styles *s, const uint8_t *payload, uint32_t size) {
    uint32_t id = ui_wire_get_u32(payload);
    if (!id) return false;
    uint32_t header = ui_wire_define_style_header(payload);
    uint32_t blocks = ui_wire_get_u16(payload + 6);
    uint32_t total = 0, off = header;
    for (uint32_t b = 0; b < blocks; b++) {
        total += ui_wire_get_u16(payload + off + 4);
        off += ui_wire_get_u16(payload + off);
    }
    ui_style_def d = {.id = id};
    d.blocks = malloc((blocks ? blocks : 1) * sizeof *d.blocks);
    d.entries = malloc((total ? total : 1) * sizeof *d.entries);
    d.tokens = malloc((total ? total : 1) * sizeof *d.tokens);
    bool ok = d.blocks && d.entries && d.tokens;
    off = header;
    for (uint32_t b = 0; ok && b < blocks; b++) {
        const uint8_t *bp = payload + off;
        uint32_t bytes = ui_wire_get_u16(bp), bh = ui_wire_size_or(bp + 6, STYLE_BLOCK_HEADER);
        uint32_t n;
        ok = ui_style_decode(s, bp + bh, bytes - bh, ui_wire_get_u16(bp + 4), false,
                             d.entries + d.entry_count, &n);
        d.blocks[d.block_count++] = (ui_style_block){bp[2], d.entry_count, n};
        d.entry_count += n;
        off += bytes;
    }
    (void)size;
    for (uint32_t i = 0; ok && i < d.entry_count; i++) {
        if (d.entries[i].value.kind != UI_VALUE_TOKEN) continue;
        uint32_t t = d.entries[i].value.token;
        bool seen = false;
        for (uint32_t k = 0; k < d.token_count && !seen; k++) seen = d.tokens[k] == t;
        if (!seen) d.tokens[d.token_count++] = t;
    }
    ok = ok && grow_defs(s) &&
         note_change(&s->changed_styles, &s->changed_style_count, &s->changed_style_capacity, id);
    if (!ok) {
        free_def(s, &d);
        return false;
    }
    bool found;
    ui_style_def *slot = def_slot(s, id, &found);
    if (found) {
        free_def(s, slot);
    } else {
        if (slot->tombstone) s->def_tombstones--;
        s->def_live++;
    }
    *slot = d;
    return true;
}

bool ui_styles_destroy(ui_styles *s, uint32_t id) {
    if (!s->def_capacity || !id) return false;
    bool found;
    ui_style_def *d = def_slot(s, id, &found);
    if (!found ||
        !note_change(&s->changed_styles, &s->changed_style_count, &s->changed_style_capacity, id))
        return false;
    free_def(s, d);
    *d = (ui_style_def){.tombstone = true};
    s->def_live--;
    s->def_tombstones++;
    return true;
}

// ---- Tokens ------------------------------------------------------------------------------

static ui_token *token_slot(const ui_styles *s, uint32_t id, bool *found) {
    uint32_t mask = s->token_capacity - 1;
    ui_token *reuse = NULL;
    for (uint32_t i = hash_id(id) & mask;; i = (i + 1) & mask) {
        ui_token *t = &s->tokens[i];
        if (!t->id && !t->tombstone) {
            *found = false;
            return reuse ? reuse : t;
        }
        if (t->tombstone) {
            if (!reuse) reuse = t;
        } else if (t->id == id) {
            *found = true;
            return t;
        }
    }
}

static bool grow_tokens(ui_styles *s) {
    if ((s->token_live + s->token_tombstones + 1) * 4 <= s->token_capacity * 3) return true;
    uint32_t cap = s->token_capacity ? s->token_capacity * 2 : 64;
    if (s->token_live * 4 < s->token_capacity) cap = s->token_capacity;
    ui_token *old = s->tokens;
    uint32_t old_cap = s->token_capacity;
    s->tokens = calloc(cap, sizeof *s->tokens);
    if (!s->tokens) {
        s->tokens = old;
        return false;
    }
    s->token_capacity = cap;
    s->token_tombstones = 0;
    for (uint32_t i = 0; i < old_cap; i++) {
        bool found;
        if (old[i].id) *token_slot(s, old[i].id, &found) = old[i];
    }
    free(old);
    return true;
}

static const ui_value *token_of(const ui_styles *s, uint32_t id) {
    if (!s->token_capacity) return NULL;
    bool found;
    const ui_token *t = token_slot(s, id, &found);
    return found ? &t->value : NULL;
}

bool ui_styles_set_token(ui_styles *s, const uint8_t *payload, uint32_t size) {
    uint32_t id = ui_wire_get_u32(payload);
    const uint8_t *e = payload + UI_WIRE_SIZE_SET_TOKEN_HEADER;
    (void)size;
    ui_value v;
    if (!id || !ui_style_kind_words(e[2]) || !read_value(e, &v)) return false;
    if (v.kind != UI_VALUE_UNSET && !ui_style_token_value_valid(&v)) return false;
    if (!grow_tokens(s) ||
        !note_change(&s->changed_tokens, &s->changed_token_count, &s->changed_token_capacity, id) ||
        !intern_value(s, &v))
        return false;
    bool found;
    ui_token *t = token_slot(s, id, &found);
    if (found) ui_styles_release_value(s, &t->value);
    if (v.kind == UI_VALUE_UNSET) {
        if (found) {
            *t = (ui_token){.tombstone = true};
            s->token_live--;
            s->token_tombstones++;
        }
        return true;
    }
    if (!found) {
        if (t->tombstone) s->token_tombstones--;
        s->token_live++;
    }
    *t = (ui_token){.id = id, .value = v};
    return true;
}

// Floyd's cycle detection: `fast` follows the chain two steps at a time and `slow` one, so
// they meet if and only if the chain loops.
bool ui_styles_token_value(const ui_styles *s, const ui_value *v, ui_value *out) {
    if (v->kind != UI_VALUE_TOKEN) {
        *out = *v;
        return true;
    }
    uint32_t slow = v->token, fast = v->token;
    for (;;) {
        for (int step = 0; step < 2; step++) {
            const ui_value *t = token_of(s, fast);
            if (!t) return false;
            if (t->kind != UI_VALUE_TOKEN) {
                *out = *t;
                return true;
            }
            fast = t->token;
        }
        slow = token_of(s, slow)->token; // fast already passed it: a token reference
        if (slow == fast) return false;
    }
}

// ---- Resolution --------------------------------------------------------------------------

void ui_layout_style_init(ui_layout_style *l) {
    memset(l, 0, sizeof *l);
    l->shrink = 1;
    l->basis.unit = UI_LEN_AUTO;
    for (int a = 0; a < 2; a++) {
        l->size[a].unit = UI_LEN_AUTO;
        l->min[a].unit = UI_LEN_AUTO;
        l->max[a].unit = UI_LEN_NONE;
    }
    for (int i = 0; i < 4; i++) l->inset[i].unit = UI_LEN_AUTO;
}

void ui_computed_init(ui_computed *c) {
    memset(c, 0, sizeof *c);
    c->opacity = 1;
    c->text = (ui_text_values){.color = {0, 0, 0, 1}, .size = 16, .weight = 400};
    ui_layout_style_init(&c->layout);
}

// The initial values, once (every property's `initial`).
static const ui_computed *initial_values(void) {
    static ui_computed init;
    static bool ready;
    if (!ready) {
        ui_computed_init(&init);
        ready = true;
    }
    return &init;
}

void ui_computed_start(ui_computed *out, const ui_computed *parent) {
    *out = *initial_values();
    if (parent) out->text = parent->text;
}

static ui_length length_of(const ui_value *v) {
    switch (v->kind) {
    case UI_VALUE_PX: return (ui_length){v->number, UI_LEN_PX};
    case UI_VALUE_PERCENT: return (ui_length){v->number, UI_LEN_PERCENT};
    case UI_VALUE_NONE: return (ui_length){0, UI_LEN_NONE};
    default: return (ui_length){0, UI_LEN_AUTO};
    }
}

static int anim_of(uint16_t prop) {
    switch (prop) {
    case UI_STYLE_BACKGROUND_COLOR: return UI_ANIM_BACKGROUND;
    case UI_STYLE_BORDER_COLOR: return UI_ANIM_BORDER_COLOR;
    case UI_STYLE_CORNER_RADIUS: return UI_ANIM_RADIUS;
    case UI_STYLE_OPACITY: return UI_ANIM_OPACITY;
    case UI_STYLE_TEXT_COLOR: return UI_ANIM_TEXT_COLOR;
    case UI_STYLE_LAYOUT: return UI_ANIM_LAYOUT;
    default: return -1;
    }
}

static void apply_layout(ui_layout_style *l, uint16_t prop, const ui_value *v) {
    uint8_t k = (uint8_t)v->keyword;
    switch ((ui_style_prop)prop) {
    case UI_STYLE_DISPLAY: l->display = k; break;
    case UI_STYLE_POSITION: l->position = k; break;
    case UI_STYLE_FLEX_DIRECTION: l->direction = k; break;
    case UI_STYLE_FLEX_WRAP: l->wrap = k; break;
    case UI_STYLE_JUSTIFY_CONTENT: l->justify = k; break;
    case UI_STYLE_ALIGN_ITEMS: l->align_items = k; break;
    case UI_STYLE_ALIGN_SELF: l->align_self = k; break;
    case UI_STYLE_ALIGN_CONTENT: l->align_content = k; break;
    case UI_STYLE_FLEX_GROW: l->grow = v->number; break;
    case UI_STYLE_FLEX_SHRINK: l->shrink = v->number; break;
    case UI_STYLE_FLEX_BASIS: l->basis = length_of(v); break;
    case UI_STYLE_COLUMN_GAP: l->gap[0] = length_of(v); break;
    case UI_STYLE_ROW_GAP: l->gap[1] = length_of(v); break;
    case UI_STYLE_WIDTH: l->size[0] = length_of(v); break;
    case UI_STYLE_HEIGHT: l->size[1] = length_of(v); break;
    case UI_STYLE_MIN_WIDTH: l->min[0] = length_of(v); break;
    case UI_STYLE_MIN_HEIGHT: l->min[1] = length_of(v); break;
    case UI_STYLE_MAX_WIDTH: l->max[0] = length_of(v); break;
    case UI_STYLE_MAX_HEIGHT: l->max[1] = length_of(v); break;
    case UI_STYLE_MARGIN_TOP:
    case UI_STYLE_MARGIN_RIGHT:
    case UI_STYLE_MARGIN_BOTTOM:
    case UI_STYLE_MARGIN_LEFT: l->margin[prop - UI_STYLE_MARGIN_TOP] = length_of(v); break;
    case UI_STYLE_PADDING_TOP:
    case UI_STYLE_PADDING_RIGHT:
    case UI_STYLE_PADDING_BOTTOM:
    case UI_STYLE_PADDING_LEFT: l->padding[prop - UI_STYLE_PADDING_TOP] = length_of(v); break;
    case UI_STYLE_TOP:
    case UI_STYLE_RIGHT:
    case UI_STYLE_BOTTOM:
    case UI_STYLE_LEFT: l->inset[prop - UI_STYLE_TOP] = length_of(v); break;
    case UI_STYLE_ASPECT_RATIO: l->aspect_ratio = v->kind == UI_VALUE_NUMBER ? v->number : 0; break;
    default: break;
    }
}

// Where a property's computed value lives, for inherit and initial, which copy it whole.
static bool slot_of(uint16_t prop, size_t *off, size_t *size) {
#define AT(field) (*off = offsetof(ui_computed, field), *size = sizeof(((ui_computed *)0)->field), true)
    if (prop & 0x8000u) {
        int a = anim_of(prop & 0x7FFFu);
        return a >= 0 && AT(transition[a]);
    }
    switch ((ui_style_prop)prop) {
    case UI_STYLE_BACKGROUND_COLOR: return AT(background);
    case UI_STYLE_BORDER_COLOR: return AT(border_color);
    case UI_STYLE_BORDER_WIDTH: return AT(border_width);
    case UI_STYLE_CORNER_RADIUS: return AT(corner_radius);
    case UI_STYLE_OPACITY: return AT(opacity);
    case UI_STYLE_CLIP: return AT(clip);
    case UI_STYLE_TEXT_COLOR: return AT(text.color);
    case UI_STYLE_LINE_HEIGHT: return AT(text.line_height);
    case UI_STYLE_TEXT_ALIGN: return AT(text.align);
    case UI_STYLE_MAX_LINES: return AT(max_lines);
    case UI_STYLE_FONT_FAMILY: return AT(text.family);
    case UI_STYLE_FONT_SIZE: return AT(text.size);
    case UI_STYLE_FONT_WEIGHT: return AT(text.weight);
    case UI_STYLE_FONT_SLANT: return AT(text.slant);
    case UI_STYLE_LANGUAGE: return AT(text.language);
    case UI_STYLE_DIRECTION: return AT(text.direction);
    case UI_STYLE_DISPLAY: return AT(layout.display);
    case UI_STYLE_POSITION: return AT(layout.position);
    case UI_STYLE_FLEX_DIRECTION: return AT(layout.direction);
    case UI_STYLE_FLEX_WRAP: return AT(layout.wrap);
    case UI_STYLE_JUSTIFY_CONTENT: return AT(layout.justify);
    case UI_STYLE_ALIGN_ITEMS: return AT(layout.align_items);
    case UI_STYLE_ALIGN_SELF: return AT(layout.align_self);
    case UI_STYLE_ALIGN_CONTENT: return AT(layout.align_content);
    case UI_STYLE_FLEX_GROW: return AT(layout.grow);
    case UI_STYLE_FLEX_SHRINK: return AT(layout.shrink);
    case UI_STYLE_FLEX_BASIS: return AT(layout.basis);
    case UI_STYLE_COLUMN_GAP: return AT(layout.gap[0]);
    case UI_STYLE_ROW_GAP: return AT(layout.gap[1]);
    case UI_STYLE_WIDTH: return AT(layout.size[0]);
    case UI_STYLE_HEIGHT: return AT(layout.size[1]);
    case UI_STYLE_MIN_WIDTH: return AT(layout.min[0]);
    case UI_STYLE_MIN_HEIGHT: return AT(layout.min[1]);
    case UI_STYLE_MAX_WIDTH: return AT(layout.max[0]);
    case UI_STYLE_MAX_HEIGHT: return AT(layout.max[1]);
    case UI_STYLE_MARGIN_TOP:
    case UI_STYLE_MARGIN_RIGHT:
    case UI_STYLE_MARGIN_BOTTOM:
    case UI_STYLE_MARGIN_LEFT: return AT(layout.margin[prop - UI_STYLE_MARGIN_TOP]);
    case UI_STYLE_PADDING_TOP:
    case UI_STYLE_PADDING_RIGHT:
    case UI_STYLE_PADDING_BOTTOM:
    case UI_STYLE_PADDING_LEFT: return AT(layout.padding[prop - UI_STYLE_PADDING_TOP]);
    case UI_STYLE_TOP:
    case UI_STYLE_RIGHT:
    case UI_STYLE_BOTTOM:
    case UI_STYLE_LEFT: return AT(layout.inset[prop - UI_STYLE_TOP]);
    case UI_STYLE_ASPECT_RATIO: return AT(layout.aspect_ratio);
    case UI_STYLE_LAYOUT: return false;
    }
#undef AT
    return false;
}

static bool inherited(uint16_t prop) {
    switch (prop) {
    case UI_STYLE_TEXT_COLOR:
    case UI_STYLE_LINE_HEIGHT:
    case UI_STYLE_TEXT_ALIGN:
    case UI_STYLE_FONT_FAMILY:
    case UI_STYLE_FONT_SIZE:
    case UI_STYLE_FONT_WEIGHT:
    case UI_STYLE_FONT_SLANT:
    case UI_STYLE_LANGUAGE:
    case UI_STYLE_DIRECTION: return true;
    default: return false;
    }
}

// Applies one entry; returns UI_STYLE_INHERITS_OTHER for `inherit` on a non-inherited
// property.
static uint32_t apply(const ui_styles *s, const ui_style_entry *e, const ui_computed *parent,
                      ui_computed *c) {
    ui_value v;
    if (e->value.kind == UI_VALUE_TOKEN &&
        (!ui_styles_token_value(s, &e->value, &v) || !ui_style_value_valid(e->prop, &v, false)))
        return 0; // a missing token, a cycle, or a value this property can't take: absent
    if (e->value.kind != UI_VALUE_TOKEN) v = e->value;
    if (v.kind == UI_VALUE_UNSET) return 0;
    if (v.kind == UI_VALUE_INHERIT || v.kind == UI_VALUE_INITIAL) {
        size_t off, size;
        if (!slot_of(e->prop, &off, &size)) return 0;
        const ui_computed *from = v.kind == UI_VALUE_INITIAL || !parent ? initial_values() : parent;
        memcpy((char *)c + off, (const char *)from + off, size);
        return v.kind == UI_VALUE_INHERIT && parent && !inherited(e->prop) ? UI_STYLE_INHERITS_OTHER : 0;
    }
    if (e->prop & 0x8000u) {
        int a = anim_of(e->prop & 0x7FFFu);
        if (a < 0) return 0;
        c->transition[a][0] = v.kind == UI_VALUE_SPRING ? v.spring.stiffness : 0;
        c->transition[a][1] = v.kind == UI_VALUE_SPRING ? v.spring.damping : 0;
        return 0;
    }
    switch ((ui_style_prop)e->prop) {
    case UI_STYLE_BACKGROUND_COLOR: ui_color_from_wire(v.color, c->background); break;
    case UI_STYLE_BORDER_COLOR: ui_color_from_wire(v.color, c->border_color); break;
    case UI_STYLE_BORDER_WIDTH: c->border_width = v.number; break;
    case UI_STYLE_CORNER_RADIUS: c->corner_radius = v.number; break;
    case UI_STYLE_OPACITY: c->opacity = v.number; break;
    case UI_STYLE_CLIP: c->clip = v.keyword == UI_CLIP_RECT; break;
    case UI_STYLE_TEXT_COLOR: ui_color_from_wire(v.color, c->text.color); break;
    case UI_STYLE_LINE_HEIGHT: c->text.line_height = v.kind == UI_VALUE_AUTO ? 0 : v.number; break;
    case UI_STYLE_TEXT_ALIGN: c->text.align = (uint8_t)v.keyword; break;
    case UI_STYLE_MAX_LINES: c->max_lines = v.kind == UI_VALUE_NONE ? 0 : (uint32_t)v.number; break;
    case UI_STYLE_FONT_FAMILY: c->text.family = v.string.bytes; break;
    case UI_STYLE_FONT_SIZE: c->text.size = v.number; break;
    case UI_STYLE_FONT_WEIGHT: c->text.weight = (uint16_t)v.number; break;
    case UI_STYLE_FONT_SLANT: c->text.slant = (uint8_t)v.keyword; break;
    case UI_STYLE_LANGUAGE: c->text.language = v.kind == UI_VALUE_NONE ? NULL : v.string.bytes; break;
    case UI_STYLE_DIRECTION: c->text.direction = (uint8_t)v.keyword; break;
    default: apply_layout(&c->layout, e->prop, &v); break;
    }
    return 0;
}

typedef struct block_ref {
    uint32_t key;               // specificity, then list position, then block
    const ui_style_def *def;
    const ui_style_block *block;
} block_ref;

bool ui_style_cascade(const ui_styles *s, const uint32_t *ids, uint32_t count, uint8_t state,
                      const ui_style_entry *overrides, uint32_t override_count,
                      const ui_style_entry ***list, uint32_t *len, uint32_t *cap) {
    // Matching blocks: at most 255 styles; more than 4 blocks each overflow to the heap.
    block_ref local[256], *refs = local;
    uint32_t ref_cap = UI_ARRAY_LEN(local), n = 0, entries = override_count;
    bool ok = true;
    for (uint32_t i = 0; ok && i < count; i++) {
        const ui_style_def *d = ui_styles_find(s, ids[i]);
        for (uint32_t b = 0; d && b < d->block_count; b++) {
            const ui_style_block *blk = &d->blocks[b];
            if (blk->states & ~state) continue;
            if (n == ref_cap) {
                block_ref *more = malloc(2 * ref_cap * sizeof *more);
                if (!(ok = more != NULL)) break;
                memcpy(more, refs, n * sizeof *refs);
                if (refs != local) free(refs);
                refs = more;
                ref_cap *= 2;
            }
            uint32_t spec = (uint32_t)__builtin_popcount(blk->states);
            refs[n++] = (block_ref){spec << 24 | i << 16 | (b & 0xFFFF), d, blk};
            entries += blk->count;
        }
    }
    ok = ok && ui_grow((void **)list, cap, entries ? entries : 1, sizeof **list) == UI_OK;
    if (ok) {
        // Insertion sort: lists are short and mostly in order already.
        for (uint32_t i = 1; i < n; i++) {
            block_ref r = refs[i];
            uint32_t j = i;
            while (j && refs[j - 1].key > r.key) {
                refs[j] = refs[j - 1];
                j--;
            }
            refs[j] = r;
        }
        uint32_t k = 0;
        for (uint32_t i = 0; i < n; i++) {
            const ui_style_block *blk = refs[i].block;
            for (uint32_t e = 0; e < blk->count; e++) (*list)[k++] = &refs[i].def->entries[blk->first + e];
        }
        for (uint32_t e = 0; e < override_count; e++) (*list)[k++] = &overrides[e];
        *len = k;
    }
    if (refs != local) free(refs);
    return ok;
}

uint32_t ui_style_compute(const ui_styles *s, const ui_style_entry *const *list, uint32_t count,
                          const ui_computed *parent, ui_computed *out) {
    uint32_t flags = 0;
    for (uint32_t i = 0; i < count; i++) flags |= apply(s, list[i], parent, out);
    return flags;
}

// ---- Changes -----------------------------------------------------------------------------

static bool contains(const uint32_t *list, uint32_t n, uint32_t id) {
    for (uint32_t i = 0; i < n; i++) {
        if (list[i] == id) return true;
    }
    return false;
}

bool ui_styles_take_changes(ui_styles *s, uint32_t **styles, uint32_t *style_count,
                            uint32_t **tokens, uint32_t *token_count) {
    *tokens = s->changed_tokens;
    *token_count = s->changed_token_count;
    uint32_t token_capacity = s->changed_token_capacity;
    s->changed_tokens = NULL;
    s->changed_token_count = s->changed_token_capacity = 0;
    // Tokens whose chain reaches a changed token changed too.
    for (bool grew = *token_count > 0; grew;) {
        grew = false;
        for (uint32_t i = 0; i < s->token_capacity; i++) {
            const ui_token *t = &s->tokens[i];
            if (!t->id || t->value.kind != UI_VALUE_TOKEN || contains(*tokens, *token_count, t->id) ||
                !contains(*tokens, *token_count, t->value.token))
                continue;
            if (ui_grow((void **)tokens, &token_capacity, *token_count + 1, sizeof **tokens) != UI_OK)
                break;
            (*tokens)[(*token_count)++] = t->id;
            grew = true;
        }
    }
    // Styles that use any of them.
    for (uint32_t i = 0; *token_count && i < s->def_capacity; i++) {
        const ui_style_def *d = &s->defs[i];
        for (uint32_t k = 0; d->id && k < d->token_count; k++) {
            if (!contains(*tokens, *token_count, d->tokens[k])) continue;
            note_change(&s->changed_styles, &s->changed_style_count, &s->changed_style_capacity, d->id);
            break;
        }
    }
    *styles = s->changed_styles;
    *style_count = s->changed_style_count;
    s->changed_styles = NULL;
    s->changed_style_count = s->changed_style_capacity = 0;
    return *style_count || *token_count;
}
