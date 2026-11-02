#include "core/clock.h"

#include "runtime/common.h"

static uint64_t monotonic_source(void *user) {
    (void)user;
    return ui_monotonic_ns();
}

void ui_clock_init(ui_clock *c, ui_time_source source, bool paused) {
    c->source = source.now_ns ? source : (ui_time_source){monotonic_source, NULL};
    c->paused = paused;
    c->scale = 1.0;
    c->anim_base = 0;
    c->host_base = ui_clock_host_now(c);
}

uint64_t ui_clock_host_now(const ui_clock *c) { return c->source.now_ns(c->source.user); }

uint64_t ui_clock_at(const ui_clock *c, uint64_t host_ns) {
    if (c->paused || host_ns <= c->host_base) return c->anim_base;
    return c->anim_base + (uint64_t)((double)(host_ns - c->host_base) * c->scale);
}

static void rebase(ui_clock *c) {
    uint64_t host = ui_clock_host_now(c);
    c->anim_base = ui_clock_at(c, host);
    c->host_base = host;
}

void ui_clock_pause(ui_clock *c, bool paused) {
    if (c->paused == paused) return;
    rebase(c);
    c->paused = paused;
}

void ui_clock_step(ui_clock *c, uint64_t delta_ns) {
    rebase(c);
    c->anim_base += delta_ns;
}

void ui_clock_set_scale(ui_clock *c, double scale) {
    rebase(c);
    c->scale = scale;
}
