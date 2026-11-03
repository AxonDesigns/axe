// Wire protocol v0: the command buffer (logic -> core) and the event buffer (core -> logic).
// The binary format is specified in docs/command-buffer.md. Language runtimes interact with
// the core only through these two buffers.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ui/handle.h"
#include "ui/result.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_WIRE_VERSION 0u

typedef enum ui_op {
    UI_OP_CREATE_NODE = 1,
    UI_OP_DESTROY_NODE = 2,
    UI_OP_SET_RECT = 3,
    UI_OP_SET_CORNER_RADIUS = 4,
    UI_OP_SET_COLOR = 5,
    UI_OP_START_SPRING = 6,
    UI_OP_SET_TIMER = 7,
    UI_OP_SET_TEXT = 8,
    UI_OP_APPEND_TEXT = 9, // continues a set_text whose text is longer than one op holds
    UI_OP_SET_CLIP = 10,
    UI_OP_LOAD_FONT = 11,
    UI_OP_DEFINE_STYLE = 12,
    UI_OP_DESTROY_STYLE = 13,
    UI_OP_SET_STYLES = 14,
    UI_OP_SET_PROPS = 15,
    UI_OP_SET_TOKEN = 16,
    UI_OP_SET_STATE = 17,
    UI_OP_MOVE_NODE = 18,
    UI_OP_REQUEST_LAYOUT = 19,
} ui_op;

// Animatable node properties and their component counts.
typedef enum ui_property {
    UI_PROP_RECT = 1,          // x, y, width, height in logical pixels, relative to the parent
    UI_PROP_CORNER_RADIUS = 2, // logical pixels
    UI_PROP_COLOR = 3,         // sRGB, straight alpha; springs interpolate in linear light
} ui_property;

#define UI_PROP_MAX_COMPONENTS 4u

// Number of float components of a property, or 0 if the property is unknown.
uint32_t ui_property_components(ui_property property);

typedef enum ui_event_type {
    UI_EVENT_FRAME_COMMITTED = 1,
    UI_EVENT_SURFACE_RESIZED = 2,
    UI_EVENT_SURFACE_CLOSE_REQUESTED = 3,
    UI_EVENT_TIMER_FIRED = 4,
    UI_EVENT_FONT_LOADED = 5,
    UI_EVENT_LAYOUT_MEASURED = 6,
} ui_event_type;

// A rectangle in logical pixels.
typedef struct ui_rect {
    float x, y, width, height;
} ui_rect;

typedef struct ui_event {
    ui_event_type type;
    ui_handle surface;
    union {
        struct {
            uint64_t frame_id;
            // Animation-clock time (ns) at which this frame is expected on screen.
            uint64_t predicted_present_ns;
        } frame_committed;
        struct {
            float width, height; // logical pixels
            uint32_t scale_120;  // scale factor in 120ths (120 = 1.0, 150 = 1.25)
        } surface_resized;
        struct {
            uint32_t id;
            uint64_t deadline_ns; // animation-clock time the timer was set for
            uint64_t now_ns;      // animation-clock time it fired (>= deadline_ns)
        } timer_fired;
        struct {
            uint32_t id;          // the request id given to ui_cmd_load_font
            ui_result result;     // UI_OK, UI_ERR_IO (unreadable), UI_ERR_INVALID_ARGUMENT
        } font_loaded;
        // The answer to ui_cmd_request_layout; `surface` is the node's surface (null when
        // the handle is stale). The layout rect is where layout put the node; the
        // presentation rect is where it is drawn, which differs while a layout animation
        // runs. Each is given relative to the parent's border box (the parent's own layout or
        // presentation rect) and to the surface. All four are zero for a node that isn't laid
        // out: display none, or under a node that is.
        struct {
            uint32_t request_id;
            ui_handle node;
            ui_result result; // UI_OK, or UI_ERR_STALE_HANDLE (rects zero)
            ui_rect layout_in_parent, layout_in_surface;
            ui_rect presentation_in_parent, presentation_in_surface;
        } layout_measured;
    };
} ui_event;

// ---- Command writer ----------------------------------------------------------------------

typedef struct ui_cmd_writer ui_cmd_writer;

// Thread: any; a writer is then used by one thread at a time.
// Ownership: caller owns *out; free with ui_cmd_writer_destroy.
ui_result ui_cmd_writer_create(ui_cmd_writer **out);
void ui_cmd_writer_destroy(ui_cmd_writer *writer);

// Number of ops written since the last reset or take.
uint32_t ui_cmd_writer_op_count(const ui_cmd_writer *writer);

// Ops are appended in order and applied atomically, in order, at one frame boundary.
// They fail only with UI_ERR_INVALID_ARGUMENT or UI_ERR_OUT_OF_MEMORY; on failure the
// writer is unchanged. Handles are not validated here; the core validates them.
ui_result ui_cmd_create_node(ui_cmd_writer *w, ui_handle node, ui_handle parent);
ui_result ui_cmd_destroy_node(ui_cmd_writer *w, ui_handle node);
// Moves a node (with its subtree) under `parent` (a node, or a surface for a root-level
// node), before sibling `before`; a null `before` appends. Reorders when the parent stays.
ui_result ui_cmd_move_node(ui_cmd_writer *w, ui_handle node, ui_handle parent, ui_handle before);
// Asks for the node's layout and presentation rects, answered by a UI_EVENT_LAYOUT_MEASURED
// with this id once the frame that applies the request has laid the node out (at once when
// its layout is already current). Never causes a frame by itself.
ui_result ui_cmd_request_layout(ui_cmd_writer *w, ui_handle node, uint32_t request_id);
ui_result ui_cmd_set_rect(ui_cmd_writer *w, ui_handle node, float x, float y, float width,
                          float height);
ui_result ui_cmd_set_corner_radius(ui_cmd_writer *w, ui_handle node, float radius);
ui_result ui_cmd_set_color(ui_cmd_writer *w, ui_handle node, float r, float g, float b, float a);
// With `clip`, the node's text and its descendants are drawn only inside its rect
// (rectangular: rounded corners are not clipped; the node's own background is unaffected).
ui_result ui_cmd_set_clip(ui_cmd_writer *w, ui_handle node, bool clip);
// Starts or retargets a spring (mass 1) on `property`; `target` has
// ui_property_components(property) floats. A running spring keeps its velocity.
ui_result ui_cmd_start_spring(ui_cmd_writer *w, ui_handle node, ui_property property,
                              const float *target, float stiffness, float damping);

// Arms one-shot timer `id` for animation-clock time `deadline_ns`, replacing an armed timer
// with the same id. It fires once as UI_EVENT_TIMER_FIRED, even while every surface is
// idle; it does not fire while the clock is paused (unless the clock is stepped past it).
ui_result ui_cmd_set_timer(ui_cmd_writer *w, uint32_t id, uint64_t deadline_ns);
// Disarms timer `id` if armed.
ui_result ui_cmd_cancel_timer(ui_cmd_writer *w, uint32_t id);

#define UI_FONT_PATH_MAX 4096u

// Registers the font file at `path` (UTF-8, at most UI_FONT_PATH_MAX bytes) at the next
// frame boundary and answers with UI_EVENT_FONT_LOADED carrying `id`. Text everywhere is
// shaped again, since the new font can change family matches and fallbacks. Face 0 only.
ui_result ui_cmd_load_font(ui_cmd_writer *w, uint32_t id, const char *path);

// ---- Styles ----------------------------------------------------------------------------
// docs/styles.md. A style is a set of declarations, defined once by ID and applied to
// nodes by listing IDs. Precedence, lowest first: initial values; set_text's paragraph
// values (text color, line height, align, max lines); the base blocks of a node's styles
// in list order; the state blocks that match the node's state, most specific first
// (number of states), then list order, then declaration order; per-node overrides.

typedef enum ui_style_prop {
    UI_STYLE_BACKGROUND_COLOR = 1, // color
    UI_STYLE_BORDER_COLOR = 2,     // color
    UI_STYLE_BORDER_WIDTH = 3,     // px >= 0, all four sides
    UI_STYLE_CORNER_RADIUS = 4,    // px >= 0, all four corners
    UI_STYLE_OPACITY = 5,          // number in [0, 1]; applies to the node and its subtree
    UI_STYLE_CLIP = 6,             // keyword ui_clip
    UI_STYLE_TEXT_COLOR = 7,       // color: the text's default style (spans keep theirs)
    UI_STYLE_LINE_HEIGHT = 8,      // number in (0, 100] (multiple of the size), or auto
    UI_STYLE_TEXT_ALIGN = 9,       // keyword ui_text_align
    UI_STYLE_MAX_LINES = 10,       // number (whole, <= 65535; 0 = unlimited), or none
    // Inherited (docs/styles.md, "Inheritance"), with text color, line height and alignment.
    UI_STYLE_FONT_FAMILY = 11,     // string: a CSS-style family list, e.g. "Noto Sans, sans-serif"
    UI_STYLE_FONT_SIZE = 12,       // px in (0, 4096]
    UI_STYLE_FONT_WEIGHT = 13,     // number, whole, in [1, 1000]
    UI_STYLE_FONT_SLANT = 14,      // keyword ui_text_slant
    UI_STYLE_LANGUAGE = 15,        // string: a BCP 47 tag, e.g. "sr-Cyrl"; or none
    UI_STYLE_DIRECTION = 16,       // keyword ui_direction
    // Layout: flexbox with CSS semantics (docs/layout.md). Every node is a flex container;
    // sizes are border-box. Lengths are px or percent as noted.
    UI_STYLE_DISPLAY = 32,         // keyword ui_display
    UI_STYLE_POSITION = 33,        // keyword ui_position (absolute: relative to the parent)
    UI_STYLE_FLEX_DIRECTION = 34,  // keyword ui_flex_direction
    UI_STYLE_FLEX_WRAP = 35,       // keyword ui_flex_wrap
    UI_STYLE_JUSTIFY_CONTENT = 36, // keyword ui_flex_align: normal, start, end, center, space-*
    UI_STYLE_ALIGN_ITEMS = 37,     // keyword ui_flex_align: normal, start, end, center, baseline, stretch
    UI_STYLE_ALIGN_SELF = 38,      // keyword ui_flex_align: auto, start, end, center, baseline, stretch
    UI_STYLE_ALIGN_CONTENT = 39,   // keyword ui_flex_align: normal, start, end, center, stretch, space-*
    UI_STYLE_FLEX_GROW = 40,       // number >= 0
    UI_STYLE_FLEX_SHRINK = 41,     // number >= 0
    UI_STYLE_FLEX_BASIS = 42,      // px >= 0, percent >= 0, or auto
    UI_STYLE_ROW_GAP = 43,         // px or percent, >= 0
    UI_STYLE_COLUMN_GAP = 44,      // px or percent, >= 0
    UI_STYLE_WIDTH = 45,           // px or percent, >= 0, or auto
    UI_STYLE_HEIGHT = 46,
    UI_STYLE_MIN_WIDTH = 47,       // px or percent, >= 0, or auto
    UI_STYLE_MIN_HEIGHT = 48,
    UI_STYLE_MAX_WIDTH = 49,       // px or percent, >= 0, or none
    UI_STYLE_MAX_HEIGHT = 50,
    UI_STYLE_MARGIN_TOP = 51,      // px or percent, or auto
    UI_STYLE_MARGIN_RIGHT = 52,
    UI_STYLE_MARGIN_BOTTOM = 53,
    UI_STYLE_MARGIN_LEFT = 54,
    UI_STYLE_PADDING_TOP = 55,     // px or percent, >= 0
    UI_STYLE_PADDING_RIGHT = 56,
    UI_STYLE_PADDING_BOTTOM = 57,
    UI_STYLE_PADDING_LEFT = 58,
    UI_STYLE_TOP = 59,             // insets: px or percent, or auto
    UI_STYLE_RIGHT = 60,
    UI_STYLE_BOTTOM = 61,
    UI_STYLE_LEFT = 62,
    UI_STYLE_ASPECT_RATIO = 63,    // number > 0 (width / height), or auto
    // The node's layout rect (its position and size from layout). It can't be declared, only
    // transitioned: UI_STYLE_TRANSITION(UI_STYLE_LAYOUT) animates the node from where it is
    // drawn to every new layout rect (layout animations, docs/layout.md).
    UI_STYLE_LAYOUT = 64,
} ui_style_prop;

// The transition of a property: a spring that animates the property's value whenever what
// it resolves to changes, whatever the reason. Its value is UI_VALUE_SPRING, or
// UI_VALUE_NONE for no transition. Transitions exist for background, border color, corner
// radius, opacity, text color and the layout rect (UI_STYLE_LAYOUT).
#define UI_STYLE_TRANSITION(prop) ((uint16_t)(0x8000u | (uint16_t)(prop)))

typedef enum ui_value_kind {
    UI_VALUE_UNSET = 1,   // per-node overrides only: removes the override
    UI_VALUE_PX = 2,      // logical pixels
    UI_VALUE_PERCENT = 3,
    UI_VALUE_AUTO = 4,
    UI_VALUE_NONE = 5,
    UI_VALUE_NUMBER = 6,
    UI_VALUE_COLOR = 7,   // sRGB, straight alpha, clamped to [0, 1]
    UI_VALUE_KEYWORD = 8, // a property's enumerated value
    UI_VALUE_TOKEN = 9,   // the value of theme token `token` (see ui_cmd_set_token)
    UI_VALUE_SPRING = 10, // a transition: stiffness > 0, damping >= 0 (mass 1)
    UI_VALUE_STRING = 11, // UTF-8, no NUL, at most UI_STYLE_STRING_MAX bytes
    UI_VALUE_INHERIT = 12, // the parent's computed value (any property)
    UI_VALUE_INITIAL = 13, // the property's initial value (any property)
} ui_value_kind;

#define UI_STYLE_STRING_MAX 1014u // what fits an entry of 255 words

typedef struct ui_value {
    ui_value_kind kind;
    union {
        float number;      // px, percent, number
        float color[4];
        uint32_t keyword;
        uint32_t token;
        struct {
            float stiffness, damping;
        } spring;
        struct {
            const char *bytes; // the writer copies them
            uint32_t length;
        } string;
    };
} ui_value;

// Value constructors (C only: compound literals).
#ifndef __cplusplus
static inline ui_value ui_px(float v) { return (ui_value){.kind = UI_VALUE_PX, .number = v}; }
static inline ui_value ui_percent(float v) { return (ui_value){.kind = UI_VALUE_PERCENT, .number = v}; }
static inline ui_value ui_number(float v) { return (ui_value){.kind = UI_VALUE_NUMBER, .number = v}; }
static inline ui_value ui_auto(void) { return (ui_value){.kind = UI_VALUE_AUTO}; }
static inline ui_value ui_none(void) { return (ui_value){.kind = UI_VALUE_NONE}; }
static inline ui_value ui_unset(void) { return (ui_value){.kind = UI_VALUE_UNSET}; }
static inline ui_value ui_keyword(uint32_t k) { return (ui_value){.kind = UI_VALUE_KEYWORD, .keyword = k}; }
static inline ui_value ui_token_ref(uint32_t id) { return (ui_value){.kind = UI_VALUE_TOKEN, .token = id}; }
static inline ui_value ui_rgba(float r, float g, float b, float a) {
    return (ui_value){.kind = UI_VALUE_COLOR, .color = {r, g, b, a}};
}
static inline ui_value ui_transition(float stiffness, float damping) {
    return (ui_value){.kind = UI_VALUE_SPRING, .spring = {stiffness, damping}};
}
static inline ui_value ui_string(const char *s) {
    uint32_t n = 0;
    while (s && s[n]) n++;
    return (ui_value){.kind = UI_VALUE_STRING, .string = {s, n}};
}
static inline ui_value ui_inherit(void) { return (ui_value){.kind = UI_VALUE_INHERIT}; }
static inline ui_value ui_initial(void) { return (ui_value){.kind = UI_VALUE_INITIAL}; }
#endif

// The direction of a node's content: its flex rows and its text (UI_STYLE_DIRECTION).
typedef enum ui_direction {
    UI_DIRECTION_LTR = 0,
    UI_DIRECTION_RTL = 1,
} ui_direction;

typedef enum ui_display {
    UI_DISPLAY_FLEX = 0,
    UI_DISPLAY_NONE = 1, // neither laid out nor drawn, with its subtree
} ui_display;

typedef enum ui_position {
    UI_POSITION_RELATIVE = 0, // in the flex flow
    UI_POSITION_ABSOLUTE = 1, // out of it, placed by insets in the parent's padding box
} ui_position;

typedef enum ui_flex_direction {
    UI_FLEX_ROW = 0,
    UI_FLEX_ROW_REVERSE = 1,
    UI_FLEX_COLUMN = 2,
    UI_FLEX_COLUMN_REVERSE = 3,
} ui_flex_direction;

typedef enum ui_flex_wrap {
    UI_FLEX_NOWRAP = 0,
    UI_FLEX_WRAP = 1,
    UI_FLEX_WRAP_REVERSE = 2,
} ui_flex_wrap;

// Values of justify-content, align-items, align-self and align-content (each takes a
// subset). NORMAL is each property's initial value: flex-start for justify-content,
// stretch for align-items and align-content, and "the parent's align-items" (auto) for
// align-self.
typedef enum ui_flex_align {
    UI_FLEX_NORMAL = 0,
    UI_FLEX_START = 1,
    UI_FLEX_END = 2,
    UI_FLEX_CENTER = 3,
    UI_FLEX_BASELINE = 4,
    UI_FLEX_STRETCH = 5,
    UI_FLEX_SPACE_BETWEEN = 6,
    UI_FLEX_SPACE_AROUND = 7,
    UI_FLEX_SPACE_EVENLY = 8,
} ui_flex_align;

typedef enum ui_clip {
    UI_CLIP_NONE = 0,
    UI_CLIP_RECT = 1, // text and descendants are drawn only inside the node's rect
} ui_clip;

// Node states that select state blocks. Input sets them from milestone 4; until then
// ui_cmd_set_state does.
typedef enum ui_state {
    UI_STATE_HOVER = 1,
    UI_STATE_PRESSED = 2,
    UI_STATE_FOCUSED = 4,
    UI_STATE_DISABLED = 8,
    UI_STATE_SELECTED = 16,
} ui_state;

typedef struct ui_style_decl {
    uint16_t property; // ui_style_prop, or UI_STYLE_TRANSITION(prop)
    ui_value value;
} ui_style_decl;

// A block of declarations that applies when the node has every state in `states` (0: the
// base block, always).
typedef struct ui_style_block_desc {
    uint8_t states;
    const ui_style_decl *decls;
    uint32_t count;
} ui_style_block_desc;

// Defines style `id` (not 0), or redefines it: every node using it is resolved again.
// UI_ERR_INVALID_ARGUMENT for an unknown property, a kind the property doesn't accept, an
// out-of-range value, or more than 255 blocks or 65535 declarations in a block.
ui_result ui_cmd_define_style(ui_cmd_writer *w, uint32_t id, const ui_style_block_desc *blocks,
                              uint32_t block_count);
// Removes style `id`; nodes listing it resolve as if it weren't there (until redefined).
ui_result ui_cmd_destroy_style(ui_cmd_writer *w, uint32_t id);
// Replaces the node's style list (at most 255 IDs). IDs not defined yet contribute nothing
// until they are.
ui_result ui_cmd_set_styles(ui_cmd_writer *w, ui_handle node, const uint32_t *ids,
                            uint32_t count);
// Sets per-node overrides, which win over every style. UI_VALUE_UNSET removes one. Writes
// to the same property replace the earlier override.
ui_result ui_cmd_set_props(ui_cmd_writer *w, ui_handle node, const ui_style_decl *decls,
                           uint32_t count);
// Sets theme token `id` (not 0) to a value: px, percent, number, color, keyword, or another
// token (resolved when used; a cycle or a missing token makes the declaration that uses it
// count as absent). UI_VALUE_UNSET removes the token. Every node using it is resolved again.
ui_result ui_cmd_set_token(ui_cmd_writer *w, uint32_t id, ui_value value);
// Sets the node's states selected by `mask` to those in `value` (ui_state bits).
ui_result ui_cmd_set_state(ui_cmd_writer *w, ui_handle node, uint32_t mask, uint32_t value);

// ---- Text ------------------------------------------------------------------------------

#define UI_TEXT_FAMILY_DEFAULT 0xFFFFu
#define UI_TEXT_MAX_SPANS 1024u
#define UI_TEXT_MAX_STRINGS 64u     // family lists and language tags together
#define UI_TEXT_MAX_LANGUAGE 35u    // bytes per language tag
#define UI_TEXT_MAX_ELLIPSIS 64u    // bytes
#define UI_TEXT_MAX_BYTES (16u << 20)

typedef enum ui_text_direction {
    UI_TEXT_DIR_AUTO = 0, // each paragraph follows its first strong character
    UI_TEXT_DIR_LTR = 1,
    UI_TEXT_DIR_RTL = 2,
} ui_text_direction;

typedef enum ui_text_align {
    UI_TEXT_ALIGN_START = 0, // left in LTR paragraphs, right in RTL ones
    UI_TEXT_ALIGN_END = 1,
    UI_TEXT_ALIGN_LEFT = 2,
    UI_TEXT_ALIGN_RIGHT = 3,
    UI_TEXT_ALIGN_CENTER = 4,
    UI_TEXT_ALIGN_JUSTIFY = 5, // spaces stretch; a paragraph's last line is start-aligned
} ui_text_align;

typedef enum ui_text_slant {
    UI_TEXT_NORMAL = 0,
    UI_TEXT_ITALIC = 1,
    UI_TEXT_OBLIQUE = 2,
} ui_text_slant;

// The fields of set_text a caller sets explicitly (ui_text_desc.set, ui_text_span_desc.set).
// Every other field comes from the node: its computed (styled or inherited) value for the
// paragraph's defaults, and the paragraph's defaults for a span (docs/styles.md,
// "Inheritance"). Zero-initialized descriptors therefore follow the node's styles.
typedef enum ui_text_field {
    UI_TEXT_SET_FAMILY = 1u << 0,
    UI_TEXT_SET_SIZE = 1u << 1,
    UI_TEXT_SET_WEIGHT = 1u << 2,
    UI_TEXT_SET_SLANT = 1u << 3,
    UI_TEXT_SET_LANGUAGE = 1u << 4,
    UI_TEXT_SET_COLOR = 1u << 5,
    UI_TEXT_SET_LINE_HEIGHT = 1u << 6, // ui_text_desc only
    UI_TEXT_SET_ALIGN = 1u << 7,       // ui_text_desc only
    UI_TEXT_SET_DIRECTION = 1u << 8,   // ui_text_desc only: else the node's direction
    UI_TEXT_SET_SPAN_ALL = 0x3Fu,      // every field a span has
    UI_TEXT_SET_ALL = 0x1FFu,
} ui_text_field;

typedef struct ui_text_style_desc {
    uint16_t family; // index into ui_text_desc.families, or UI_TEXT_FAMILY_DEFAULT
    uint16_t weight; // 1..1000 (400 regular, 700 bold)
    uint8_t slant;   // ui_text_slant
    // 1-based index into ui_text_desc.languages; 0: none (HarfBuzz's default). Selects
    // language-specific forms, e.g. Romanian ș or Serbian Cyrillic.
    uint16_t language;
    float size;      // logical px, > 0
    float color[4];  // sRGB, straight alpha, clamped to [0, 1]
} ui_text_style_desc;

typedef struct ui_text_span_desc {
    uint32_t start, end; // UTF-8 byte offsets into the text; sorted and non-overlapping
    ui_text_style_desc style;
    uint32_t set;        // ui_text_field bits of `style` this span sets; the rest are the paragraph's
} ui_text_span_desc;

typedef struct ui_text_desc {
    const char *text;           // UTF-8; invalid sequences become U+FFFD in the core
    uint32_t length;            // bytes, at most UI_TEXT_MAX_BYTES
    ui_text_style_desc style;   // for bytes no span covers
    const ui_text_span_desc *spans;
    uint32_t span_count;        // at most UI_TEXT_MAX_SPANS
    const char *const *families; // CSS-style family lists, e.g. "Noto Sans, sans-serif"
    uint32_t family_count;
    const char *const *languages; // BCP 47 tags, e.g. "ro", "sr-Cyrl", "zh-Hant"
    uint32_t language_count;    // families + languages: at most UI_TEXT_MAX_STRINGS
    ui_text_direction direction;
    uint16_t max_lines;         // 0: unlimited
    const char *ellipsis;       // shown when max_lines cuts text; NULL: U+2026, "": none
    ui_text_align align;
    // Line box height as a multiple of style.size, e.g. 1.4. 0: from the fonts' metrics
    // (lines with tall fallback fonts get taller). Otherwise every line is the same height
    // and the baseline is placed by the default style's font. At most 100.
    float line_height;
    // The ui_text_field bits of `style`, `line_height`, `align` and `direction` set here;
    // the others come from the node's styles or its parent (0: all of them).
    uint32_t set;
} ui_text_desc;

// Sets a node's text. It wraps to the node's width and is drawn over its background, in
// the node's coordinate space (origin at its top-left). Text longer than one op holds is
// split into set_text + append_text ops in the same buffer, so the change stays atomic.
// UI_ERR_INVALID_ARGUMENT for out-of-range values or unsorted/overlapping spans.
ui_result ui_cmd_set_text(ui_cmd_writer *w, ui_handle node, const ui_text_desc *desc);

// Finalizes the header and moves the encoded bytes to the caller, leaving the writer empty
// and reusable. Ownership: *out_bytes is malloc'd; release it with free().
ui_result ui_cmd_writer_take(ui_cmd_writer *w, uint8_t **out_bytes, size_t *out_size);

// ---- Event reader ------------------------------------------------------------------------

typedef struct ui_event_reader {
    const uint8_t *data;
    size_t size;
    size_t offset;      // byte offset of the next event
    uint32_t remaining; // events left according to the header
} ui_event_reader;

// Validates the header. The reader borrows `data`, which must outlive it.
// Thread: any. Returns UI_ERR_MALFORMED for a bad header or version.
ui_result ui_event_reader_init(ui_event_reader *reader, const void *data, size_t size);
// Reads the next event. Returns UI_ERR_END after the last one, UI_ERR_MALFORMED on a framing
// error. Unknown event types are skipped.
ui_result ui_event_next(ui_event_reader *reader, ui_event *out);

#ifdef __cplusplus
}
#endif
