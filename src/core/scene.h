// The core's scene: nodes mirrored from the logic side's handles, their animated properties
// and the trees hanging off each surface. Owned by the core actor; not thread-safe.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/display_list.h"
#include "core/spring.h"
#include "runtime/common.h"
#include "runtime/handle_table.h"
#include "text/atlas.h"
#include "text/paragraph.h"

#define UI_NO_SLOT UINT32_MAX

enum { UI_SPRING_RECT = 0, UI_SPRING_RADIUS, UI_SPRING_COLOR, UI_SPRING_COUNT };

typedef struct ui_node {
    uint32_t parent;       // node slot, or UI_NO_SLOT when attached to a surface root
    uint32_t root;         // index into ui_scene.roots
    uint32_t first_child, last_child, next_sibling, prev_sibling;
    float rect[4];         // logical px relative to the parent
    float radius;
    float color[4];        // linear, straight alpha
    ui_spring springs[UI_SPRING_COUNT];
    ui_paragraph *text;    // NULL when the node has no text
} ui_node;

typedef struct ui_scene_root {
    ui_handle surface;     // null when unused
    uint32_t first_child, last_child;
    bool dirty;            // something under this root changed since the core last drew it
    bool animating;        // a spring under this root is still running
} ui_scene_root;

// Receives set_timer ops, which belong to the core rather than to any node.
typedef void (*ui_scene_timer_fn)(void *user, uint32_t id, uint64_t deadline_ns);

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

typedef struct ui_scene {
    const ui_logger *log;
    ui_handle_table handles;
    ui_node *nodes;
    uint32_t node_capacity;
    ui_scene_root roots[UI_MAX_SURFACES];
    ui_scene_stats stats;
    uint32_t *scratch; // traversal stack
    uint32_t scratch_capacity;
    ui_scene_timer_fn timer_fn;
    void *timer_user;
} ui_scene;

void ui_scene_init(ui_scene *s, const ui_logger *log);
void ui_scene_destroy(ui_scene *s);

ui_result ui_scene_add_root(ui_scene *s, ui_handle surface);
// Destroys every node under the surface.
void ui_scene_remove_root(ui_scene *s, ui_handle surface);

// Validates the whole buffer, then applies every op in order at animation time `now_ns`.
// A malformed buffer is rejected entirely (UI_ERR_MALFORMED) and nothing is applied, so a
// flush is all-or-nothing. Ops on stale handles are dropped individually and reported in
// debug builds. Every applied node op marks its root dirty; set_timer goes to timer_fn.
ui_result ui_scene_apply(ui_scene *s, const uint8_t *data, size_t size, uint64_t now_ns);

// Advances every running spring to `now_ns`. Roots with a spring that was running are
// marked dirty (the settling frame still changes the picture); `animating` is updated to
// whether one is still running.
void ui_scene_animate(ui_scene *s, uint64_t now_ns);

// The root for a surface, or NULL. Callers read and clear `dirty`.
ui_scene_root *ui_scene_root_of(ui_scene *s, ui_handle surface);

// Appends the surface's tree to `dl` in paint order (parents before children; a node's
// background, then its text). Text wraps to the node's width at the list's scale; its
// glyphs are rasterized into the atlas on first use. `text` may be NULL (no text drawn).
ui_result ui_scene_build(ui_scene *s, ui_handle surface, ui_display_list *dl,
                         const ui_scene_text *text);
