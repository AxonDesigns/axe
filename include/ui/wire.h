// Wire protocol v0: the command buffer (logic -> core) and the event buffer (core -> logic).
// The binary format is specified in docs/command-buffer.md. Language runtimes interact with
// the core only through these two buffers.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ui/handle.h"
#include "ui/result.h"
#include "ui/wire_ids.h"

#ifdef __cplusplus
extern "C" {
#endif

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
        // First in every event buffer: the animation clock when the core sent it, never behind
        // a time the buffer reports. Logic keeps time from it (the language runtimes' timers).
        struct {
            uint64_t now_ns;
        } clock;
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
