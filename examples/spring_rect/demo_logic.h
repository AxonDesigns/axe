// Temporary C logic actor standing in for the language runtime. It talks to the core only
// through the wire buffers: it creates one rounded rect when the surface appears and every
// two seconds (of animation-clock time, through a core timer) springs it to the next
// corner and color.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ui/ui.h"

#define DEMO_ANCHORS 4u
#define DEMO_PERIOD_NS 2000000000ull
#define DEMO_TIMER_ID 1u
#define DEMO_STIFFNESS 170.0f
#define DEMO_DAMPING 18.25f // damping ratio 0.7: a small overshoot, settled well within 2 s

typedef struct demo_logic {
    ui_handle_allocator *handles;
    ui_handle surface;
    ui_handle node;
    float width, height;   // logical surface size
    bool timer_started;
    uint64_t next_switch_ns;
    uint32_t anchor;
    uint64_t period_ns;    // time between springs; DEMO_PERIOD_NS by default
    bool stall;            // UI_STALL_LOGIC: block 500 ms while each spring is in flight
    bool stall_pending;
} demo_logic;

ui_result demo_logic_init(demo_logic *d);
void demo_logic_fini(demo_logic *d);
// Fills the callbacks of a ui_logic_desc whose user pointer is `d`.
ui_logic_desc demo_logic_desc(demo_logic *d);

// Geometry shared with the tests: the rect (x, y, w, h, logical px) at `anchor` on a surface
// of the given logical size, and the anchor's sRGB color.
void demo_logic_rect(uint32_t anchor, float width, float height, float out[4]);
void demo_logic_color(uint32_t anchor, float out[4]);
float demo_logic_radius(float width, float height);
