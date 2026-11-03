// Layout: flexbox with CSS semantics (docs/layout.md). Every node is a flex container; a
// node with text is a text leaf, whose paragraph fills its content box and sizes it (as a
// CSS block holding only text); a surface is a column container the size of the surface.
// Results are cached per node by the inputs they were computed for, and dirty flags
// propagate up, so a change lays out again only the nodes on its path and whatever their
// new sizes affect.
//
// Runs on the core thread at the frame boundary, after commands apply and styles resolve.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define UI_LAYOUT_CACHE 6u

// One cached computation: the inputs (sizes known from the parent, the parent's content
// size for percentages, the available space) and what came out.
typedef struct ui_layout_cache {
    float known[2];   // NaN: not fixed by the parent
    float parent[2];  // NaN: indefinite
    float avail[2];   // >= 0: definite; UI_AVAIL_MIN_CONTENT or UI_AVAIL_MAX_CONTENT
    float size[2];    // border box
    float baseline;   // first baseline from the border box's top; NaN: none
    bool used;
    uint8_t intrinsic; // measured ignoring the node's own size, min and max: 1 in x, 2 in y
} ui_layout_cache;

#define UI_AVAIL_MIN_CONTENT (-1.f)
#define UI_AVAIL_MAX_CONTENT (-2.f)

typedef struct ui_layout_node {
    float rect[4];    // x, y relative to the parent's border box; width, height (logical px)
    float text[4];    // where its text goes: the content box, relative to the border box
    float baseline;   // first baseline from the top of the border box; NaN: none
    bool dirty;       // its inputs changed since it was last laid out
    uint8_t next;     // measurement entry replaced next
    ui_layout_cache measured[UI_LAYOUT_CACHE];
    ui_layout_cache final; // the last full layout (children positioned)
} ui_layout_node;

typedef struct ui_layout_stats {
    uint64_t computed;   // node computations that ran (cache misses)
    uint64_t cache_hits;
    uint64_t passes;     // roots laid out
} ui_layout_stats;
