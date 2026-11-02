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
} ui_event_type;

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
ui_result ui_cmd_set_rect(ui_cmd_writer *w, ui_handle node, float x, float y, float width,
                          float height);
ui_result ui_cmd_set_corner_radius(ui_cmd_writer *w, ui_handle node, float radius);
ui_result ui_cmd_set_color(ui_cmd_writer *w, ui_handle node, float r, float g, float b, float a);
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

// ---- Text ------------------------------------------------------------------------------

#define UI_TEXT_FAMILY_DEFAULT 0xFFFFu
#define UI_TEXT_MAX_SPANS 1024u
#define UI_TEXT_MAX_FAMILIES 64u
#define UI_TEXT_MAX_ELLIPSIS 64u    // bytes
#define UI_TEXT_MAX_BYTES (16u << 20)

typedef enum ui_text_direction {
    UI_TEXT_DIR_AUTO = 0, // each paragraph follows its first strong character
    UI_TEXT_DIR_LTR = 1,
    UI_TEXT_DIR_RTL = 2,
} ui_text_direction;

typedef enum ui_text_slant {
    UI_TEXT_NORMAL = 0,
    UI_TEXT_ITALIC = 1,
    UI_TEXT_OBLIQUE = 2,
} ui_text_slant;

typedef struct ui_text_style_desc {
    uint16_t family; // index into ui_text_desc.families, or UI_TEXT_FAMILY_DEFAULT
    uint16_t weight; // 1..1000 (400 regular, 700 bold)
    uint8_t slant;   // ui_text_slant
    float size;      // logical px, > 0
    float color[4];  // sRGB, straight alpha, clamped to [0, 1]
} ui_text_style_desc;

typedef struct ui_text_span_desc {
    uint32_t start, end; // UTF-8 byte offsets into the text; sorted and non-overlapping
    ui_text_style_desc style;
} ui_text_span_desc;

typedef struct ui_text_desc {
    const char *text;           // UTF-8; invalid sequences become U+FFFD in the core
    uint32_t length;            // bytes, at most UI_TEXT_MAX_BYTES
    ui_text_style_desc style;   // for bytes no span covers
    const ui_text_span_desc *spans;
    uint32_t span_count;        // at most UI_TEXT_MAX_SPANS
    const char *const *families; // CSS-style family lists, e.g. "Noto Sans, sans-serif"
    uint32_t family_count;      // at most UI_TEXT_MAX_FAMILIES
    ui_text_direction direction;
    uint16_t max_lines;         // 0: unlimited
    const char *ellipsis;       // shown when max_lines cuts text; NULL: U+2026, "": none
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
