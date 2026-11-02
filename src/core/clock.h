// The global animation clock: maps host time to animation time with pause, step and scale.
// Owned by the core; every function runs on the main (core) thread.
#pragma once

#include <stdint.h>

#include "ui/app.h"

typedef struct ui_clock {
    ui_time_source source;
    bool paused;
    double scale;
    uint64_t anim_base; // animation time at host_base
    uint64_t host_base;
} ui_clock;

void ui_clock_init(ui_clock *c, ui_time_source source, bool paused);
uint64_t ui_clock_host_now(const ui_clock *c);
// Animation time corresponding to host time `host_ns` (e.g. a predicted present time).
uint64_t ui_clock_at(const ui_clock *c, uint64_t host_ns);
void ui_clock_pause(ui_clock *c, bool paused);
void ui_clock_step(ui_clock *c, uint64_t delta_ns);
void ui_clock_set_scale(ui_clock *c, double scale);
