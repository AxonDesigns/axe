// Damped harmonic springs (mass 1), solved analytically so results do not depend on the
// frame rate or on how a time span is split into frames.
#pragma once

#include <stdint.h>

typedef struct ui_spring {
    bool active;
    uint32_t components;
    double stiffness, damping;
    double position[4], velocity[4], target[4];
    uint64_t time_ns; // animation time of position/velocity
} ui_spring;

// Starts the spring toward `target`, or retargets it if already running: position and
// velocity carry over, so an interrupted spring never jumps or stops dead. `current` is
// used only when the spring is idle.
void ui_spring_start(ui_spring *s, uint32_t components, const float *current, const float *target,
                     double stiffness, double damping, uint64_t now_ns);

// Advances to `now_ns` (earlier times are ignored). Returns true when every component is
// within `epsilon` of the target with near-zero velocity; the spring then snaps to the
// target and deactivates.
bool ui_spring_advance(ui_spring *s, uint64_t now_ns, double epsilon);

// Closed-form displacement/velocity after `t` seconds; exposed for tests.
void ui_spring_solve(double stiffness, double damping, double x0, double v0, double t, double *x,
                     double *v);
