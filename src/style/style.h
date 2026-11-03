// Interned styles, theme tokens, and their resolution into each node's computed values
// (docs/styles.md). Owned by the scene, core thread only.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "runtime/common.h"
#include "ui/wire.h"

typedef struct ui_style_entry {
    uint16_t prop;  // ui_style_prop or UI_STYLE_TRANSITION(prop)
    ui_value value; // as on the wire: colors are sRGB here; strings are interned
} ui_style_entry;

// An interned string. Equal strings share one, so values compare by pointer; style entries,
// tokens and nodes hold references. Values refer to the bytes (ui_istr_of recovers it).
typedef struct ui_istr {
    uint32_t refs;
    uint32_t len;
    uint32_t hash;
    bool garbage;   // queued for collection (its references reached 0)
    char bytes[];   // NUL-terminated
} ui_istr;

static inline const ui_istr *ui_istr_of(const char *bytes) {
    return (const ui_istr *)(const void *)(bytes - __builtin_offsetof(ui_istr, bytes));
}

static inline uint32_t ui_istr_len(const char *bytes) { return bytes ? ui_istr_of(bytes)->len : 0; }

// Equal contents, for strings interned in different tables (NULL equals only NULL).
static inline bool ui_istr_eq(const char *a, const char *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    uint32_t n = ui_istr_len(a);
    return n == ui_istr_len(b) && !__builtin_memcmp(a, b, n);
}

typedef struct ui_style_block {
    uint8_t states; // ui_state bits that must all be set; 0: base
    uint32_t first, count;
} ui_style_block;

typedef struct ui_style_def {
    uint32_t id;    // 0: free slot
    bool tombstone;
    ui_style_block *blocks;
    uint32_t block_count;
    ui_style_entry *entries;
    uint32_t entry_count;
    uint32_t *tokens; // ids of the tokens its entries reference directly
    uint32_t token_count;
} ui_style_def;

typedef struct ui_token {
    uint32_t id;    // 0: free slot
    bool tombstone;
    ui_value value;
} ui_token;

typedef struct ui_styles {
    ui_style_def *defs;   // open addressing by id, power-of-two capacity
    uint32_t def_capacity, def_live, def_tombstones;
    ui_token *tokens;     // the same, for tokens
    uint32_t token_capacity, token_live, token_tombstones;
    // Changes since the last resolution: their users are resolved again.
    uint32_t *changed_styles;
    uint32_t changed_style_count, changed_style_capacity;
    uint32_t *changed_tokens;
    uint32_t changed_token_count, changed_token_capacity;
    // Interned strings: open addressing by hash. Strings whose references reached 0 wait in
    // `garbage` until ui_styles_collect, so values computed before a change stay readable
    // until resolution has replaced them.
    ui_istr **strings;
    uint32_t string_capacity, string_live, string_tombstones;
    ui_istr **garbage;
    uint32_t garbage_count, garbage_capacity;
} ui_styles;

// Transitionable properties (UI_STYLE_TRANSITION): the paint properties, then the layout
// rect (layout animations).
enum {
    UI_ANIM_BACKGROUND = 0,
    UI_ANIM_BORDER_COLOR,
    UI_ANIM_RADIUS,
    UI_ANIM_OPACITY,
    UI_ANIM_TEXT_COLOR,
    UI_ANIM_PAINT_COUNT,
    UI_ANIM_LAYOUT = UI_ANIM_PAINT_COUNT,
    UI_ANIM_COUNT,
};

// The inherited values (docs/styles.md, "Inheritance"): a node without a declaration of
// one takes its parent's.
typedef struct ui_text_values {
    float color[4];       // linear light, straight alpha
    const char *family;   // interned family list; NULL: the default family
    const char *language; // interned BCP 47 tag; NULL: none
    float size;           // logical px
    float line_height;    // multiple of the size; 0: from the fonts' metrics
    uint16_t weight;
    uint8_t slant;        // ui_text_slant
    uint8_t align;        // ui_text_align
    uint8_t direction;    // ui_direction
} ui_text_values;

// A length as resolved: its unit, and the value in px or percent. Zero is 0 px.
enum { UI_LEN_PX = 0, UI_LEN_PERCENT, UI_LEN_AUTO, UI_LEN_NONE };
typedef struct ui_length {
    float value;
    uint8_t unit;
} ui_length;

// Layout values (docs/layout.md); ui_layout_style_init gives CSS's initial values.
typedef struct ui_layout_style {
    uint8_t display, position, direction, wrap;
    uint8_t justify, align_items, align_self, align_content; // ui_flex_align
    float grow, shrink;
    float aspect_ratio;    // 0: auto
    ui_length basis;
    ui_length gap[2];      // along x (column-gap) and y (row-gap)
    ui_length size[2];     // width, height
    ui_length min[2];      // auto: the automatic minimum size
    ui_length max[2];      // none by default
    ui_length margin[4];   // top, right, bottom, left
    ui_length padding[4];
    ui_length inset[4];
} ui_layout_style;

// What a node's styles, state and overrides resolve to.
typedef struct ui_computed {
    float background[4];   // linear light, straight alpha
    float border_color[4][4]; // per side: top, right, bottom, left
    float border_width[4];    // logical px, per side
    float corner_radius[4];   // logical px: top left, top right, bottom right, bottom left
    float opacity;
    bool clip;
    ui_text_values text;   // inherited
    uint32_t max_lines;    // 0: unlimited
    float transition[UI_ANIM_COUNT][2]; // stiffness, damping; stiffness 0: none
    ui_layout_style layout;
} ui_computed;

void ui_styles_init(ui_styles *s);
void ui_styles_free(ui_styles *s);

// Applying ops. Each returns false when the op is dropped (invalid, or out of memory). The
// payloads' framing was validated (ui_style_walk).
bool ui_styles_define(ui_styles *s, const uint8_t *payload, uint32_t size);
bool ui_styles_destroy(ui_styles *s, uint32_t id);
bool ui_styles_set_token(ui_styles *s, const uint8_t *payload, uint32_t size);
const ui_style_def *ui_styles_find(const ui_styles *s, uint32_t id);

// Decodes `count` entries from p[0..bytes) (validated framing) into out (room for count).
// Entries with an unknown property or kind are skipped. False if a known one is invalid
// (nothing is then kept). Strings are interned: the caller owns a reference to each.
bool ui_style_decode(ui_styles *s, const uint8_t *p, uint32_t bytes, uint32_t count, bool override,
                     ui_style_entry *out, uint32_t *out_count);

// Interned strings. Intern returns the shared bytes with a new reference (NULL: out of
// memory); release drops one. A value holding a string (ui_value) is retained and released
// with the _value variants, which ignore other kinds.
const char *ui_styles_intern(ui_styles *s, const char *bytes, uint32_t len);
void ui_styles_retain(ui_styles *s, const char *bytes);
void ui_styles_release(ui_styles *s, const char *bytes);
void ui_styles_release_value(ui_styles *s, const ui_value *v);
// Frees the strings nothing refers to any more. Call when no computed value can still
// point at one (after resolution).
void ui_styles_collect(ui_styles *s);

// Checks the framing of `count` entries in p[0..bytes): every entry at least its header,
// none past the end. *end is where they stop.
bool ui_style_walk(const uint8_t *p, uint32_t bytes, uint32_t count, uint32_t *end);
// Checks the framing of a define_style payload (blocks and their entries).
bool ui_style_walk_define(const uint8_t *payload, uint32_t size);

// The token a value refers to resolved through chains of tokens; false for a missing token
// or a cycle (the declaration then counts as absent).
bool ui_styles_token_value(const ui_styles *s, const ui_value *v, ui_value *out);

// What resolution reads definitions and tokens from: the styles, or the styles as a command
// buffer not yet applied would leave them (core/preview.h). `over` has the buffer's
// definitions and tokens; the ids in `gone_*` were destroyed or unset by it.
typedef struct ui_style_view {
    const ui_styles *styles;
    const ui_styles *over; // NULL: the styles alone
    const uint32_t *gone_defs, *gone_tokens;
    uint32_t gone_def_count, gone_token_count;
} ui_style_view;

static inline ui_style_view ui_style_view_of(const ui_styles *s) { return (ui_style_view){.styles = s}; }
const ui_style_def *ui_style_view_find(const ui_style_view *v, uint32_t id);
bool ui_style_view_token_value(const ui_style_view *v, const ui_value *value, ui_value *out);

// Every property's initial value.
void ui_computed_init(ui_computed *c);
// CSS's initial values: auto sizes, basis, minimums and insets; no maximums; flex 0 1 auto;
// zero margins, padding and gaps.
void ui_layout_style_init(ui_layout_style *l);

// A node's computed values come in two steps (docs/styles.md, "Inheritance"):
//  1. Its cascade: the entries that apply to it, in order of increasing precedence (the
//     matching blocks of its styles by specificity, then list order, then declaration order;
//     then its overrides). It changes only with the node's styles, state and overrides, or a
//     style's definition. *list is grown as needed (`*cap` entries); false if out of memory.
//  2. Computing: starting values (ui_computed_start: initial values, and the parent's for
//     inherited properties; the caller then puts set_text's in), then every entry of the
//     cascade in order. Tokens are read then, so a token change only computes again.
bool ui_style_cascade(const ui_style_view *v, const uint32_t *ids, uint32_t count, uint8_t state,
                      const ui_style_entry *overrides, uint32_t override_count,
                      const ui_style_entry ***list, uint32_t *len, uint32_t *cap);
void ui_computed_start(ui_computed *out, const ui_computed *parent);
// Returns UI_STYLE_INHERITS_OTHER when a non-inherited property took `inherit`: the node
// then follows every change of its parent, not only of the inherited values. *text_set
// (if not NULL) receives the UI_TEXT_SET_* bits of the text values an entry decided.
enum { UI_STYLE_INHERITS_OTHER = 1 };
uint32_t ui_style_compute(const ui_style_view *v, const ui_style_entry *const *list, uint32_t count,
                          const ui_computed *parent, ui_computed *out, uint32_t *text_set);

// The set of tokens that changed since the last call, closed over tokens referring to them
// (a token whose chain reaches a changed token changed too), and the styles whose entries
// use any of them, appended to the changed styles. Returns false if nothing changed.
// `tokens` receives the closure (malloc'd; free it).
bool ui_styles_take_changes(ui_styles *s, uint32_t **styles, uint32_t *style_count,
                            uint32_t **tokens, uint32_t *token_count);
