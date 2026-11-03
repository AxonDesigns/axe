// The core's scene: nodes mirrored from the logic side's handles, their animated properties
// and the trees hanging off each surface. Owned by the core actor; not thread-safe.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/display_list.h"
#include "core/spring.h"
#include "core/text_op.h"
#include "layout/layout.h"
#include "runtime/common.h"
#include "runtime/handle_table.h"
#include "style/style.h"
#include "text/atlas.h"
#include "text/paragraph.h"

#define UI_NO_SLOT UINT32_MAX

// Animated node properties. The transitioned ones come in the order of UI_ANIM_* (style.h),
// one after UI_SPRING_RECT.
enum {
    UI_SPRING_RECT = 0,     // set_rect's frame (start_spring)
    UI_SPRING_COLOR,        // background
    UI_SPRING_BORDER_COLOR,
    UI_SPRING_RADIUS,
    UI_SPRING_OPACITY,
    UI_SPRING_TEXT_COLOR,
    UI_SPRING_LAYOUT,       // the presentation rect, toward the layout rect
    UI_SPRING_COUNT,
};
static_assert(UI_SPRING_COLOR == 1 + UI_ANIM_BACKGROUND && UI_SPRING_TEXT_COLOR == 1 + UI_ANIM_TEXT_COLOR &&
                  UI_SPRING_LAYOUT == 1 + UI_ANIM_LAYOUT,
              "springs follow the style transitions");

// A node's springs, allocated the first time one of its properties animates.
typedef struct ui_node_anim {
    ui_spring springs[UI_SPRING_COUNT];
    // start_spring asked for this spring: it starts (or retargets) with these parameters
    // at the next resolution, whatever transition the styles declare.
    bool requested[UI_SPRING_COUNT];
    float requested_params[UI_SPRING_COUNT][2];
} ui_node_anim;

typedef struct ui_node {
    uint32_t parent;       // node slot, or UI_NO_SLOT when attached to a surface root
    uint32_t root;         // index into ui_scene.roots
    uint32_t first_child, last_child, next_sibling, prev_sibling;
    // The presentation rect: where it is drawn, in logical px relative to the parent's
    // presentation rect. Its layout rect (layout.rect) unless a layout transition moves it.
    float rect[4];
    // set_rect's frame: the absolute position and size it overrides, which a rect spring
    // animates (docs/layout.md).
    float frame[4];
    bool framed;
    ui_layout_node layout;
    bool placed;           // drawn at a layout rect before: a new one may animate
    bool rebased;          // moved under another parent: place it at the next layout
    // What is drawn. Each follows its resolved value in `computed`, through a spring while
    // one runs.
    float color[4];        // background: linear, straight alpha
    float border_color[4];
    float border_width;    // logical px
    float radius;
    float opacity;
    float text_color[4];   // the text's default style (spans keep their own colors)
    bool clip;             // text and descendants are clipped to the rect
    ui_node_anim *anim;
    ui_paragraph *text;    // NULL when the node has no text
    // Styles: the node's list, overrides and state, and what they resolve to.
    uint32_t *styles;
    ui_style_entry *overrides;
    uint16_t style_count, override_count, override_capacity;
    uint8_t state;         // ui_state bits
    uint8_t jump;          // 1 << UI_SPRING_*: a set_* op set it (it jumps unless transitioned)
    bool style_dirty;      // resolve at the next ui_scene_resolve
    bool resolved;         // resolved before: changes may animate from now on
    ui_computed computed;
} ui_node;

typedef struct ui_scene_root {
    ui_handle surface;     // null when unused
    uint32_t first_child, last_child;
    bool dirty;            // something under this root changed since the core last drew it
    bool animating;        // a spring under this root is still running
    float scale;           // the surface's scale (the core keeps it current); text shapes at it
    bool layout_dirty;     // a node under it changed what layout depends on
    float layout_size[2];  // the surface size it was last laid out for
    float layout_scale;
} ui_scene_root;

// Receives set_timer ops, which belong to the core rather than to any node.
typedef void (*ui_scene_timer_fn)(void *user, uint32_t id, uint64_t deadline_ns);
// Receives load_font ops with a valid path (not NUL-terminated); `offset` is the op's byte
// offset in its buffer, which identifies what was prepared for it.
typedef void (*ui_scene_font_fn)(void *user, uint32_t id, const char *path, uint32_t len,
                                 uint32_t offset);

// A set_text decoded and shaped before its buffer is applied (by the worker threads that
// prepare buffers). Applying adopts the source and the shaping at the node's scale.
typedef struct ui_scene_text_prep {
    uint32_t offset;       // byte offset of the set_text op in its buffer
    bool decoded;          // false: preparation didn't run; the op is decoded when applied
    ui_text_op op;         // decoded; its source reference is consumed when applied
    float scales[UI_MAX_SURFACES];
    ui_shaped_paragraph shaped[UI_MAX_SURFACES]; // per scale; moved out when adopted
    uint32_t scale_count;
} ui_scene_text_prep;

typedef struct ui_scene_prepared {
    ui_scene_text_prep *texts; // in buffer order
    uint32_t text_count;
} ui_scene_prepared;

typedef struct ui_scene_stats {
    uint64_t applied, dropped, rejected;
    uint64_t text_invalid; // ill-formed UTF-8 subparts replaced in set_text
} ui_scene_stats;

// What drawing text needs, owned by the core: fonts and shaping, the atlas, the rasterizer.
typedef struct ui_scene_text {
    ui_text_context *ctx;
    ui_atlas *atlas;
    ui_raster *raster;
} ui_scene_text;

typedef struct ui_build_level ui_build_level;

// A request_layout op awaiting its answer (the handle may have gone stale since).
typedef struct ui_scene_request {
    ui_handle node;
    uint32_t id;
} ui_scene_request;

typedef struct ui_scene {
    const ui_logger *log;
    ui_handle_table handles;
    ui_node *nodes;
    uint32_t node_capacity;
    ui_scene_root roots[UI_MAX_SURFACES];
    ui_scene_stats stats;
    ui_layout_stats layout_stats;
    ui_styles styles;
    uint32_t *style_dirty;    // slots to resolve (their style_dirty is set)
    uint32_t style_dirty_count, style_dirty_capacity;
    uint32_t *scratch; // traversal stack
    uint32_t scratch_capacity;
    ui_build_level *levels;   // build: one per ancestor level
    uint32_t level_capacity;
    ui_scene_timer_fn timer_fn;
    void *timer_user;
    ui_scene_font_fn font_fn;
    void *font_user;
    ui_scene_request *requests; // request_layout ops in the order applied; the core answers
    uint32_t request_count, request_capacity; // and removes them
} ui_scene;

void ui_scene_init(ui_scene *s, const ui_logger *log);
void ui_scene_destroy(ui_scene *s);

ui_result ui_scene_add_root(ui_scene *s, ui_handle surface);
// Destroys every node under the surface.
void ui_scene_remove_root(ui_scene *s, ui_handle surface);

// Checks a command buffer's framing without applying anything: UI_ERR_MALFORMED when the
// buffer would be rejected.
ui_result ui_scene_validate(const uint8_t *data, size_t size);

// Validates the whole buffer, then applies every op in order at animation time `now_ns`.
// A malformed buffer is rejected entirely (UI_ERR_MALFORMED) and nothing is applied, so a
// flush is all-or-nothing. Ops on stale handles are dropped individually and reported in
// debug builds. Every applied node op marks its root dirty; set_timer goes to timer_fn.
ui_result ui_scene_apply(ui_scene *s, const uint8_t *data, size_t size, uint64_t now_ns);
// The same, using what was prepared for the buffer's set_text ops (NULL: nothing).
ui_result ui_scene_apply_prepared(ui_scene *s, const uint8_t *data, size_t size, uint64_t now_ns,
                                  const ui_scene_prepared *prep);

// Checks a load_font payload's path: non-empty, at most UI_FONT_PATH_MAX bytes, no NUL,
// valid UTF-8.
bool ui_scene_font_path_valid(const uint8_t *payload, const char **path, uint32_t *len);

// Marks every root with text dirty (the fonts changed: text is shaped again, from what was
// prepared or by the text context's generation check).
void ui_scene_fonts_changed(ui_scene *s);

// Resolves the styles of every node whose list, overrides or state changed, or that uses a
// style or token that changed. A changed value jumps, or starts its transition at `now_ns`.
// Call after applying, before animating.
void ui_scene_resolve(ui_scene *s, uint64_t now_ns);

// Advances every running spring to `now_ns`. Roots with a spring that was running are
// marked dirty (the settling frame still changes the picture); `animating` is updated to
// whether one is still running.
void ui_scene_animate(ui_scene *s, uint64_t now_ns);

// The live node a handle names, or NULL.
ui_node *ui_scene_node(ui_scene *s, ui_handle h);
// The handle of the live node in `slot`.
ui_handle ui_scene_handle_of(const ui_scene *s, uint32_t slot);

// Lays out the surface's tree if anything under it changed or the surface's size or scale
// did (layout/layout.h), at animation time `now_ns`: layout transitions start then. Returns
// true if it ran.
bool ui_scene_layout(ui_scene *s, ui_handle surface, float width, float height, float scale,
                     ui_text_context *text, uint64_t now_ns);
// Implemented in layout/flex.c.
void ui_layout_root(ui_scene *s, uint32_t root, float width, float height, float scale,
                    ui_text_context *text, ui_layout_stats *stats, uint64_t now_ns);
// Called by layout for a node whose layout rect changed, or that needs placing: what is
// drawn follows, through the node's layout transition when it has one.
void ui_scene_place(ui_scene *s, uint32_t slot, uint64_t now_ns);

// The rects a layout_measured event reports for a node: its layout rect and its
// presentation rect, each relative to its parent and to the surface (x, y, width, height).
// All zero for a node that isn't laid out (display none, or under such a node). False, with
// the rects zero, for a stale handle; *surface is then null.
bool ui_scene_measure(const ui_scene *s, ui_handle node, float rects[4][4], ui_handle *surface);

// The root for a surface, or NULL. Callers read and clear `dirty`.
ui_scene_root *ui_scene_root_of(ui_scene *s, ui_handle surface);

// Appends the surface's tree to `dl` in paint order (parents before children; a node's
// background and border, then its text). A node with opacity below 1 draws with its
// subtree into a layer. Boxes at rest snap to whole physical pixels (origin and size each
// rounded); a box that moves, or whose ancestor moves, draws where it is. Text wraps to the
// node's width at the list's scale; its glyphs are rasterized into the atlas on first use.
// `text` may be NULL (no text drawn).
ui_result ui_scene_build(ui_scene *s, ui_handle surface, ui_display_list *dl,
                         const ui_scene_text *text);
