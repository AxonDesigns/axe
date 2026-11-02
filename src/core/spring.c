#include "core/spring.h"

#include <math.h>

void ui_spring_solve(double stiffness, double damping, double x0, double v0, double t, double *x,
                     double *v) {
    double w0 = sqrt(stiffness);
    double zeta = damping / (2.0 * w0);
    if (fabs(zeta - 1.0) < 1e-6) {
        // Critically damped: x = e^{-w0 t}(A + B t).
        double b = v0 + w0 * x0;
        double e = exp(-w0 * t);
        *x = e * (x0 + b * t);
        *v = e * (v0 - w0 * b * t);
    } else if (zeta < 1.0) {
        // Underdamped: x = e^{-a t}(A cos wd t + B sin wd t).
        double a = zeta * w0;
        double wd = w0 * sqrt(1.0 - zeta * zeta);
        double b = (v0 + a * x0) / wd;
        double e = exp(-a * t), c = cos(wd * t), s = sin(wd * t);
        *x = e * (x0 * c + b * s);
        *v = e * (v0 * c - (a * b + wd * x0) * s);
    } else {
        // Overdamped: x = C1 e^{r1 t} + C2 e^{r2 t}.
        double root = sqrt(zeta * zeta - 1.0);
        double r1 = -w0 * (zeta - root), r2 = -w0 * (zeta + root);
        double c2 = (v0 - r1 * x0) / (r2 - r1);
        double c1 = x0 - c2;
        double e1 = exp(r1 * t), e2 = exp(r2 * t);
        *x = c1 * e1 + c2 * e2;
        *v = r1 * c1 * e1 + r2 * c2 * e2;
    }
}

void ui_spring_start(ui_spring *s, uint32_t components, const float *current, const float *target,
                     double stiffness, double damping, uint64_t now_ns) {
    if (s->active && s->components == components) {
        // Bring the running spring to `now` so the retarget starts from the true state.
        ui_spring_advance(s, now_ns, 0.0);
    } else {
        for (uint32_t i = 0; i < components; i++) {
            s->position[i] = current[i];
            s->velocity[i] = 0.0;
        }
        s->time_ns = now_ns;
    }
    s->active = true;
    s->components = components;
    s->stiffness = stiffness;
    s->damping = damping;
    for (uint32_t i = 0; i < components; i++) s->target[i] = target[i];
}

bool ui_spring_advance(ui_spring *s, uint64_t now_ns, double epsilon) {
    if (!s->active) return true;
    if (now_ns > s->time_ns) {
        double t = (double)(now_ns - s->time_ns) * 1e-9;
        for (uint32_t i = 0; i < s->components; i++) {
            double x, v;
            ui_spring_solve(s->stiffness, s->damping, s->position[i] - s->target[i],
                            s->velocity[i], t, &x, &v);
            s->position[i] = s->target[i] + x;
            s->velocity[i] = v;
        }
        s->time_ns = now_ns;
    }
    if (epsilon <= 0.0) return false;
    for (uint32_t i = 0; i < s->components; i++) {
        if (fabs(s->position[i] - s->target[i]) > epsilon || fabs(s->velocity[i]) > epsilon * 10.0)
            return false;
    }
    for (uint32_t i = 0; i < s->components; i++) {
        s->position[i] = s->target[i];
        s->velocity[i] = 0.0;
    }
    s->active = false;
    return true;
}
