// Interned styles, theme tokens, and their resolution into each node's computed values
// (docs/styles.md). Owned by the scene, core thread only.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "runtime/common.h"
#include "ui/wire.h"

typedef struct ui_style_entry {
    uint16_t prop;  // ui_style_prop or UI_STYLE_TRANSITION(prop)
    ui_value value; // as on the wire: colors are sRGB here
} ui_style_entry;

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

// Text values that styles set; the rest come from the node's set_text.
enum {
    UI_TEXT_SET_COLOR = 1,
    UI_TEXT_SET_LINE_HEIGHT = 2,
    UI_TEXT_SET_ALIGN = 4,
    UI_TEXT_SET_MAX_LINES = 8,
};

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
    float border_color[4];
    float border_width;    // logical px
    float corner_radius;   // logical px
    float opacity;
    bool clip;
    uint8_t text_set;      // UI_TEXT_SET_* bits
    float text_color[4];
    float line_height;     // 0: from the fonts' metrics
    uint8_t text_align;    // ui_text_align
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
// Entries with an unknown property or kind are skipped. False if a known one is invalid.
bool ui_style_decode(const uint8_t *p, uint32_t bytes, uint32_t count, bool override,
                     ui_style_entry *out, uint32_t *out_count);

// Checks the framing of `count` entries in p[0..bytes): every entry at least its header,
// none past the end. *end is where they stop.
bool ui_style_walk(const uint8_t *p, uint32_t bytes, uint32_t count, uint32_t *end);
// Checks the framing of a define_style payload (blocks and their entries).
bool ui_style_walk_define(const uint8_t *payload, uint32_t size);

// The token a value refers to resolved through chains of tokens; false for a missing token
// or a cycle (the declaration then counts as absent).
bool ui_styles_token_value(const ui_styles *s, const ui_value *v, ui_value *out);

void ui_computed_init(ui_computed *c);
// CSS's initial values: auto sizes, basis, minimums and insets; no maximums; flex 0 1 auto;
// zero margins, padding and gaps.
void ui_layout_style_init(ui_layout_style *l);

// Resolves a node: initial values; the matching blocks of its styles by specificity (number
// of states), then list order, then declaration order; then `overrides`.
void ui_style_resolve(const ui_styles *s, const uint32_t *ids, uint32_t count, uint8_t state,
                      const ui_style_entry *overrides, uint32_t override_count,
                      ui_computed *out);

// The set of tokens that changed since the last call, closed over tokens referring to them
// (a token whose chain reaches a changed token changed too), and the styles whose entries
// use any of them, appended to the changed styles. Returns false if nothing changed.
// `tokens` receives the closure (malloc'd; free it).
bool ui_styles_take_changes(ui_styles *s, uint32_t **styles, uint32_t *style_count,
                            uint32_t **tokens, uint32_t *token_count);
