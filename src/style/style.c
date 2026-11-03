#include "style/style.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "core/color.h"
#include "wire/gen/wire_gen.h"

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

// ---- Decoding --------------------------------------------------------------------------

// Interns a decoded string value in place (the buffer's bytes become the shared ones).
static bool intern_value(ui_styles *s, ui_value *v) {
    if (v->kind != UI_VALUE_STRING) return true;
    const char *b = ui_styles_intern(s, v->string.bytes, v->string.length);
    if (!b) return false;
    v->string.bytes = b;
    return true;
}

bool ui_style_decode(ui_styles *s, const uint8_t *p, uint32_t count, bool override, ui_style_entry *out,
                     uint32_t *out_count) {
    uint32_t n = 0;
    bool ok = true;
    for (uint32_t i = 0; ok && i < count; i++) {
        const uint8_t *e = p;
        p += ui_wire_style_entry_size(e);
        ui_wire_style_entry h;
        ui_wire_style_entry_read(e, UI_WIRE_STYLE_ENTRY_SIZE, &h);
        // Newer writers' properties and kinds are skipped, not errors.
        if (!ui_wire_style_prop_known(h.property) || !ui_wire_value_kind_words(h.kind)) continue;
        ui_value v;
        ok = ui_wire_style_value_read(e, &v) && ui_wire_style_value_valid(h.property, &v, override) &&
             intern_value(s, &v);
        uint16_t prop = h.property;
        if (ok && v.kind == UI_VALUE_COLOR) ui_color_from_wire(v.color, v.color); // linear, once
        if (ok) out[n++] = ui_style_entry_of(prop, v);
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

// A block's header and where its entries are.
static const uint8_t *read_block(const uint8_t *bp, ui_wire_style_block *b) {
    ui_wire_style_block_read(bp, ui_wire_style_block_header_size(bp), b);
    ui_wire_style_block_tail t;
    ui_wire_style_block_tail_of(bp, b, &t);
    return t.entries;
}

bool ui_styles_define(ui_styles *s, const uint8_t *payload, uint32_t *out_id) {
    ui_wire_define_style op;
    ui_wire_define_style_read(payload, ui_wire_define_style_header_size(payload), &op);
    ui_wire_define_style_tail tail;
    ui_wire_define_style_tail_of(payload, &op, &tail);
    uint32_t id = *out_id = op.id;
    if (ui_wire_define_style_check(&op)) return false;
    uint32_t blocks = op.block_count, total = 0;
    const uint8_t *bp = tail.blocks;
    for (uint32_t b = 0; b < blocks; b++) {
        ui_wire_style_block blk;
        read_block(bp, &blk);
        total += blk.entry_count;
        bp += ui_wire_style_block_size(bp);
    }
    ui_style_def d = {.id = id};
    d.blocks = malloc((blocks ? blocks : 1) * sizeof *d.blocks);
    d.entries = malloc((total ? total : 1) * sizeof *d.entries);
    d.tokens = malloc((total ? total : 1) * sizeof *d.tokens);
    bool ok = d.blocks && d.entries && d.tokens;
    bp = tail.blocks;
    for (uint32_t b = 0; ok && b < blocks; b++) {
        ui_wire_style_block blk;
        const uint8_t *entries = read_block(bp, &blk);
        uint32_t n;
        ok = ui_style_decode(s, entries, blk.entry_count, false, d.entries + d.entry_count, &n);
        d.blocks[d.block_count++] = (ui_style_block){blk.states, d.entry_count, n};
        d.entry_count += n;
        bp += ui_wire_style_block_size(bp);
    }
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

bool ui_styles_set_token(ui_styles *s, const uint8_t *payload, uint32_t *out_id) {
    ui_wire_set_token op;
    ui_wire_set_token_read(payload, UI_WIRE_SET_TOKEN_SIZE, &op);
    ui_wire_set_token_tail tail;
    ui_wire_set_token_tail_of(payload, &op, &tail);
    ui_wire_style_entry h;
    ui_wire_style_entry_read(tail.entry, UI_WIRE_STYLE_ENTRY_SIZE, &h);
    uint32_t id = *out_id = op.id;
    ui_value v;
    if (ui_wire_set_token_check(&op) || !ui_wire_value_kind_words(h.kind) || !ui_wire_style_value_read(tail.entry, &v))
        return false;
    if (v.kind != UI_VALUE_UNSET && !ui_wire_style_token_value_valid(&v)) return false;
    if (v.kind == UI_VALUE_COLOR) ui_color_from_wire(v.color, v.color); // linear, as entries
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

// ---- Views -------------------------------------------------------------------------------

static bool listed(const uint32_t *ids, uint32_t n, uint32_t id) {
    for (uint32_t i = 0; i < n; i++) {
        if (ids[i] == id) return true;
    }
    return false;
}

const ui_style_def *ui_style_view_find(const ui_style_view *v, uint32_t id) {
    if (v->over) {
        const ui_style_def *d = ui_styles_find(v->over, id);
        if (d) return d;
        if (listed(v->gone_defs, v->gone_def_count, id)) return NULL;
    }
    return ui_styles_find(v->styles, id);
}

static const ui_value *view_token(const ui_style_view *v, uint32_t id) {
    if (v->over) {
        const ui_value *t = token_of(v->over, id);
        if (t) return t;
        if (listed(v->gone_tokens, v->gone_token_count, id)) return NULL;
    }
    return token_of(v->styles, id);
}

// Floyd's cycle detection, as in ui_styles_token_value.
bool ui_style_view_token_value(const ui_style_view *v, const ui_value *value, ui_value *out) {
    if (value->kind != UI_VALUE_TOKEN) {
        *out = *value;
        return true;
    }
    uint32_t slow = value->token, fast = value->token;
    for (;;) {
        for (int step = 0; step < 2; step++) {
            const ui_value *t = view_token(v, fast);
            if (!t) return false;
            if (t->kind != UI_VALUE_TOKEN) {
                *out = *t;
                return true;
            }
            fast = t->token;
        }
        slow = view_token(v, slow)->token;
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
    case UI_STYLE_BORDER_TOP_WIDTH:
    case UI_STYLE_BORDER_RIGHT_WIDTH:
    case UI_STYLE_BORDER_BOTTOM_WIDTH:
    case UI_STYLE_BORDER_LEFT_WIDTH: return AT(border_width[prop - UI_STYLE_BORDER_TOP_WIDTH]);
    case UI_STYLE_BORDER_TOP_COLOR:
    case UI_STYLE_BORDER_RIGHT_COLOR:
    case UI_STYLE_BORDER_BOTTOM_COLOR:
    case UI_STYLE_BORDER_LEFT_COLOR: return AT(border_color[prop - UI_STYLE_BORDER_TOP_COLOR]);
    case UI_STYLE_CORNER_RADIUS_TOP_LEFT:
    case UI_STYLE_CORNER_RADIUS_TOP_RIGHT:
    case UI_STYLE_CORNER_RADIUS_BOTTOM_RIGHT:
    case UI_STYLE_CORNER_RADIUS_BOTTOM_LEFT: return AT(corner_radius[prop - UI_STYLE_CORNER_RADIUS_TOP_LEFT]);
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
    case UI_STYLE_CONTENT_ANIMATION: return AT(content_animation);
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
    case UI_STYLE_LAYOUT:
    case UI_STYLE_MARGIN_START:
    case UI_STYLE_MARGIN_END:
    case UI_STYLE_PADDING_START:
    case UI_STYLE_PADDING_END:
    case UI_STYLE_INSET_START:
    case UI_STYLE_INSET_END:
    case UI_STYLE_BORDER_START_WIDTH:
    case UI_STYLE_BORDER_END_WIDTH:
    case UI_STYLE_BORDER_START_COLOR:
    case UI_STYLE_BORDER_END_COLOR:
    case UI_STYLE_CORNER_RADIUS_TOP_START:
    case UI_STYLE_CORNER_RADIUS_TOP_END:
    case UI_STYLE_CORNER_RADIUS_BOTTOM_START:
    case UI_STYLE_CORNER_RADIUS_BOTTOM_END: return false; // logical: mapped (ui_style_compute)
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
// The UI_TEXT_SET_* bit of a text property (0 for the others).
static uint32_t text_field(uint16_t prop) {
    switch (prop) {
    case UI_STYLE_FONT_FAMILY: return UI_TEXT_SET_FAMILY;
    case UI_STYLE_FONT_SIZE: return UI_TEXT_SET_SIZE;
    case UI_STYLE_FONT_WEIGHT: return UI_TEXT_SET_WEIGHT;
    case UI_STYLE_FONT_SLANT: return UI_TEXT_SET_SLANT;
    case UI_STYLE_LANGUAGE: return UI_TEXT_SET_LANGUAGE;
    case UI_STYLE_TEXT_COLOR: return UI_TEXT_SET_COLOR;
    case UI_STYLE_LINE_HEIGHT: return UI_TEXT_SET_LINE_HEIGHT;
    case UI_STYLE_TEXT_ALIGN: return UI_TEXT_SET_ALIGN;
    case UI_STYLE_DIRECTION: return UI_TEXT_SET_DIRECTION;
    default: return 0;
    }
}

// apply()'s result besides UI_STYLE_INHERITS_OTHER: the entry set a value.
enum { APPLIED = 0x100 };

// The entry's value through its token (false: absent, as a missing or invalid token).
static bool entry_value(const ui_style_view *view, const ui_style_entry *e, ui_value *v) {
    if (e->value.kind != UI_VALUE_TOKEN) {
        *v = e->value;
        return v->kind != UI_VALUE_UNSET;
    }
    if (!ui_style_view_token_value(view, &e->value, v)) return false;
    // A token's color was checked when it was set; what remains is whether the property
    // takes a color (the color families, and text color).
    if (v->kind == UI_VALUE_COLOR)
        return (e->family & (UI_FAMILY_BACKGROUND | UI_FAMILY_BORDER_COLOR)) || e->prop == UI_STYLE_TEXT_COLOR;
    return ui_wire_style_value_valid(e->prop, v, false);
}

static uint32_t apply(const ui_style_view *view, const ui_style_entry *e, const ui_computed *parent,
                      ui_computed *c, uint32_t *text_set) {
    ui_value v;
    if (!entry_value(view, e, &v)) return 0; // a missing token, a cycle, or a value it can't take: absent
    *text_set |= text_field(e->prop);
    if (v.kind == UI_VALUE_INHERIT || v.kind == UI_VALUE_INITIAL) {
        size_t off, size;
        if (!slot_of(e->prop, &off, &size)) return 0;
        const ui_computed *from = v.kind == UI_VALUE_INITIAL || !parent ? initial_values() : parent;
        memcpy((char *)c + off, (const char *)from + off, size);
        return APPLIED | (v.kind == UI_VALUE_INHERIT && parent && !inherited(e->prop) ? UI_STYLE_INHERITS_OTHER : 0);
    }
    if (e->prop & 0x8000u) {
        int a = anim_of(e->prop & 0x7FFFu);
        if (a < 0) return 0;
        c->transition[a][0] = v.kind == UI_VALUE_SPRING ? v.spring.stiffness : 0;
        c->transition[a][1] = v.kind == UI_VALUE_SPRING ? v.spring.damping : 0;
        return APPLIED;
    }
    switch ((ui_style_prop)e->prop) {
    case UI_STYLE_BACKGROUND_COLOR: memcpy(c->background, v.color, sizeof c->background); break;
    case UI_STYLE_BORDER_COLOR:
        for (int i = 0; i < 4; i++) memcpy(c->border_color[i], v.color, sizeof c->border_color[i]);
        break;
    case UI_STYLE_BORDER_WIDTH:
        for (int i = 0; i < 4; i++) c->border_width[i] = v.number;
        break;
    case UI_STYLE_CORNER_RADIUS:
        for (int i = 0; i < 4; i++) c->corner_radius[i] = v.number;
        break;
    case UI_STYLE_BORDER_TOP_WIDTH:
    case UI_STYLE_BORDER_RIGHT_WIDTH:
    case UI_STYLE_BORDER_BOTTOM_WIDTH:
    case UI_STYLE_BORDER_LEFT_WIDTH: c->border_width[e->prop - UI_STYLE_BORDER_TOP_WIDTH] = v.number; break;
    case UI_STYLE_BORDER_TOP_COLOR:
    case UI_STYLE_BORDER_RIGHT_COLOR:
    case UI_STYLE_BORDER_BOTTOM_COLOR:
    case UI_STYLE_BORDER_LEFT_COLOR:
        memcpy(c->border_color[e->prop - UI_STYLE_BORDER_TOP_COLOR], v.color, sizeof c->border_color[0]);
        break;
    case UI_STYLE_CORNER_RADIUS_TOP_LEFT:
    case UI_STYLE_CORNER_RADIUS_TOP_RIGHT:
    case UI_STYLE_CORNER_RADIUS_BOTTOM_RIGHT:
    case UI_STYLE_CORNER_RADIUS_BOTTOM_LEFT: c->corner_radius[e->prop - UI_STYLE_CORNER_RADIUS_TOP_LEFT] = v.number; break;
    case UI_STYLE_OPACITY: c->opacity = v.number; break;
    case UI_STYLE_CLIP: c->clip = v.keyword == UI_CLIP_RECT; break;
    case UI_STYLE_TEXT_COLOR: memcpy(c->text.color, v.color, sizeof c->text.color); break;
    case UI_STYLE_LINE_HEIGHT: c->text.line_height = v.kind == UI_VALUE_AUTO ? 0 : v.number; break;
    case UI_STYLE_TEXT_ALIGN: c->text.align = (uint8_t)v.keyword; break;
    case UI_STYLE_MAX_LINES: c->max_lines = v.kind == UI_VALUE_NONE ? 0 : (uint32_t)v.number; break;
    case UI_STYLE_FONT_FAMILY: c->text.family = v.string.bytes; break;
    case UI_STYLE_FONT_SIZE: c->text.size = v.number; break;
    case UI_STYLE_FONT_WEIGHT: c->text.weight = (uint16_t)v.number; break;
    case UI_STYLE_FONT_SLANT: c->text.slant = (uint8_t)v.keyword; break;
    case UI_STYLE_LANGUAGE: c->text.language = v.kind == UI_VALUE_NONE ? NULL : v.string.bytes; break;
    case UI_STYLE_DIRECTION: c->text.direction = (uint8_t)v.keyword; break;
    case UI_STYLE_CONTENT_ANIMATION: c->content_animation = (uint8_t)v.keyword; break;
    default: apply_layout(&c->layout, e->prop, &v); break;
    }
    return APPLIED;
}

// Logical sides (docs/styles.md, "Logical properties"): the inline sides of margins,
// padding, insets, border widths and colors, and the top and bottom pairs of corners, have
// a start and an end: left and right in a left-to-right node, right and left in a
// right-to-left one. Each group pairs a left and a right property with its logical ones.
enum { GROUPS = 7 };
static const uint16_t GROUP_START[GROUPS] = {
    UI_STYLE_MARGIN_START, UI_STYLE_PADDING_START, UI_STYLE_INSET_START, UI_STYLE_BORDER_START_WIDTH,
    UI_STYLE_BORDER_START_COLOR, UI_STYLE_CORNER_RADIUS_TOP_START, UI_STYLE_CORNER_RADIUS_BOTTOM_START};
static const uint16_t GROUP_END[GROUPS] = {
    UI_STYLE_MARGIN_END, UI_STYLE_PADDING_END, UI_STYLE_INSET_END, UI_STYLE_BORDER_END_WIDTH,
    UI_STYLE_BORDER_END_COLOR, UI_STYLE_CORNER_RADIUS_TOP_END, UI_STYLE_CORNER_RADIUS_BOTTOM_END};
static const uint16_t GROUP_LEFT[GROUPS] = {
    UI_STYLE_MARGIN_LEFT, UI_STYLE_PADDING_LEFT, UI_STYLE_LEFT, UI_STYLE_BORDER_LEFT_WIDTH,
    UI_STYLE_BORDER_LEFT_COLOR, UI_STYLE_CORNER_RADIUS_TOP_LEFT, UI_STYLE_CORNER_RADIUS_BOTTOM_LEFT};
static const uint16_t GROUP_RIGHT[GROUPS] = {
    UI_STYLE_MARGIN_RIGHT, UI_STYLE_PADDING_RIGHT, UI_STYLE_RIGHT, UI_STYLE_BORDER_RIGHT_WIDTH,
    UI_STYLE_BORDER_RIGHT_COLOR, UI_STYLE_CORNER_RADIUS_TOP_RIGHT, UI_STYLE_CORNER_RADIUS_BOTTOM_RIGHT};
// The shorthand that sets both sides of a group too (0: none).
static const uint16_t GROUP_BOTH[GROUPS] = {0, 0, 0, UI_STYLE_BORDER_WIDTH, UI_STYLE_BORDER_COLOR,
                                            UI_STYLE_CORNER_RADIUS, UI_STYLE_CORNER_RADIUS};

ui_style_entry ui_style_entry_of(uint16_t prop, ui_value value) {
    ui_style_entry e = {.prop = prop, .family = (uint8_t)ui_style_family(prop), .logical = -1, .value = value};
    for (int g = 0; g < GROUPS; g++) {
        if (prop == GROUP_START[g]) e.logical = (int8_t)(g * 2);
        if (prop == GROUP_END[g]) e.logical = (int8_t)(g * 2 + 1);
        if (prop == GROUP_LEFT[g] || prop == GROUP_BOTH[g]) e.physical |= (uint16_t)(1u << (g * 2));
        if (prop == GROUP_RIGHT[g] || prop == GROUP_BOTH[g]) e.physical |= (uint16_t)(1u << (g * 2 + 1));
    }
    return e;
}

typedef struct block_ref {
    uint32_t key;               // specificity, then list position, then block
    const ui_style_def *def;
    const ui_style_block *block;
} block_ref;

bool ui_style_cascade(const ui_style_view *v, const uint32_t *ids, uint32_t count, uint8_t state,
                      const ui_style_entry *overrides, uint32_t override_count,
                      const ui_style_entry ***list, uint32_t *len, uint32_t *cap) {
    // Matching blocks: at most 255 styles; more than 4 blocks each overflow to the heap.
    block_ref local[256], *refs = local;
    uint32_t ref_cap = UI_ARRAY_LEN(local), n = 0, entries = override_count;
    bool ok = true;
    for (uint32_t i = 0; ok && i < count; i++) {
        const ui_style_def *d = ui_style_view_find(v, ids[i]);
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

uint32_t ui_style_family(uint16_t prop) {
    switch (prop) {
    case UI_STYLE_BACKGROUND_COLOR: return UI_FAMILY_BACKGROUND;
    case UI_STYLE_BORDER_COLOR:
    case UI_STYLE_BORDER_TOP_COLOR:
    case UI_STYLE_BORDER_RIGHT_COLOR:
    case UI_STYLE_BORDER_BOTTOM_COLOR:
    case UI_STYLE_BORDER_LEFT_COLOR:
    case UI_STYLE_BORDER_START_COLOR:
    case UI_STYLE_BORDER_END_COLOR: return UI_FAMILY_BORDER_COLOR;
    case UI_STYLE_CORNER_RADIUS:
    case UI_STYLE_CORNER_RADIUS_TOP_LEFT:
    case UI_STYLE_CORNER_RADIUS_TOP_RIGHT:
    case UI_STYLE_CORNER_RADIUS_BOTTOM_RIGHT:
    case UI_STYLE_CORNER_RADIUS_BOTTOM_LEFT:
    case UI_STYLE_CORNER_RADIUS_TOP_START:
    case UI_STYLE_CORNER_RADIUS_TOP_END:
    case UI_STYLE_CORNER_RADIUS_BOTTOM_START:
    case UI_STYLE_CORNER_RADIUS_BOTTOM_END: return UI_FAMILY_RADIUS;
    case UI_STYLE_OPACITY: return UI_FAMILY_OPACITY;
    case UI_STYLE_TEXT_COLOR:
    case UI_STYLE_LINE_HEIGHT:
    case UI_STYLE_TEXT_ALIGN:
    case UI_STYLE_FONT_FAMILY:
    case UI_STYLE_FONT_SIZE:
    case UI_STYLE_FONT_WEIGHT:
    case UI_STYLE_FONT_SLANT:
    case UI_STYLE_LANGUAGE: return UI_FAMILY_TEXT;
    default: return UI_FAMILY_OTHER; // direction, the rest, and transitions
    }
}

static uint32_t compute(const ui_style_view *v, const ui_style_entry *const *list, uint32_t count,
                        const ui_computed *parent, uint32_t families, ui_computed *out, uint32_t *text_set) {
    uint32_t flags = 0, set = 0;
    // Per group and side: the position of the last physical entry that set it, and the last
    // logical entry with a value (start, end). Logical ones are mapped once the node's
    // direction is known, and win where they came later.
    // Indexed by group * 2 + side (as ui_style_entry's logical and physical).
    uint32_t physical_at[GROUPS * 2] = {0}, logical_at[GROUPS * 2]; // position + 1; 0: none
    const ui_style_entry *logical[GROUPS * 2];
    uint32_t logical_seen = 0; // bit per index: logical[] and logical_at[] hold an entry
    for (uint32_t i = 0; i < count; i++) {
        const ui_style_entry *e = list[i];
        if (!(e->family & families)) continue;
        if (e->logical >= 0) {
            ui_value value;
            if (entry_value(v, e, &value)) {
                logical[e->logical] = e;
                logical_at[e->logical] = i + 1;
                logical_seen |= 1u << e->logical;
            }
            continue;
        }
        uint32_t r = apply(v, e, parent, out, &set);
        flags |= r & UI_STYLE_INHERITS_OTHER;
        for (uint32_t bits = (r & APPLIED) ? e->physical : 0; bits; bits &= bits - 1)
            physical_at[__builtin_ctz(bits)] = i + 1;
    }
    int rtl = logical_seen && out->text.direction == UI_DIRECTION_RTL;
    for (; logical_seen; logical_seen &= logical_seen - 1) {
        int at = __builtin_ctz(logical_seen), g = at / 2;
        int physical = (at % 2) ^ rtl; // start: left in ltr, right in rtl
        if (logical_at[at] < physical_at[g * 2 + physical]) continue; // a later physical entry
        ui_style_entry mapped = ui_style_entry_of(physical ? GROUP_RIGHT[g] : GROUP_LEFT[g], logical[at]->value);
        flags |= apply(v, &mapped, parent, out, &set) & UI_STYLE_INHERITS_OTHER;
    }
    if (text_set) *text_set = set;
    return flags;
}

uint32_t ui_style_compute(const ui_style_view *v, const ui_style_entry *const *list, uint32_t count,
                          const ui_computed *parent, ui_computed *out, uint32_t *text_set) {
    return compute(v, list, count, parent, UI_FAMILY_ALL, out, text_set);
}

void ui_computed_start_families(ui_computed *out, const ui_computed *parent, uint32_t families) {
    if (families == UI_FAMILY_ALL) {
        ui_computed_start(out, parent);
        return;
    }
    const ui_computed *init = initial_values();
    if (families & UI_FAMILY_BACKGROUND) memcpy(out->background, init->background, sizeof out->background);
    if (families & UI_FAMILY_BORDER_COLOR) memcpy(out->border_color, init->border_color, sizeof out->border_color);
    if (families & UI_FAMILY_RADIUS) memcpy(out->corner_radius, init->corner_radius, sizeof out->corner_radius);
    if (families & UI_FAMILY_OPACITY) out->opacity = init->opacity;
    if (families & UI_FAMILY_TEXT) {
        uint8_t direction = out->text.direction; // not in the family
        out->text = (parent ? parent : init)->text;
        out->text.direction = direction;
    }
}

uint32_t ui_style_compute_families(const ui_style_view *v, const ui_style_entry *const *list, uint32_t count,
                                   const ui_computed *parent, uint32_t families, ui_computed *out,
                                   uint32_t *text_set) {
    return compute(v, list, count, parent, families, out, text_set);
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
    *styles = s->changed_styles;
    *style_count = s->changed_style_count;
    s->changed_styles = NULL;
    s->changed_style_count = s->changed_style_capacity = 0;
    return *style_count || *token_count;
}
