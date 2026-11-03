// Color helpers shared by the scene, text and styles. Colors arrive on the wire as sRGB
// with straight alpha; the core keeps them in linear light.
#pragma once

#include <math.h>

static inline float ui_srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static inline float ui_clamp01(float v) { return fminf(fmaxf(v, 0.f), 1.f); }

// sRGB straight-alpha components (finite) to linear light, each clamped to [0, 1].
static inline void ui_color_from_wire(const float in[4], float out[4]) {
    for (int i = 0; i < 3; i++) out[i] = ui_srgb_to_linear(ui_clamp01(in[i]));
    out[3] = ui_clamp01(in[3]);
}
