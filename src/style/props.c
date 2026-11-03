#include "style/props.h"

#include <math.h>

uint32_t ui_style_kind_words(uint8_t kind) {
    switch (kind) {
    case UI_VALUE_UNSET:
    case UI_VALUE_AUTO:
    case UI_VALUE_NONE: return 1;
    case UI_VALUE_PX:
    case UI_VALUE_PERCENT:
    case UI_VALUE_NUMBER:
    case UI_VALUE_KEYWORD:
    case UI_VALUE_TOKEN: return 2;
    case UI_VALUE_SPRING: return 3;
    case UI_VALUE_COLOR: return 5;
    default: return 0;
    }
}

static bool transitionable(uint16_t p) {
    return p == UI_STYLE_BACKGROUND_COLOR || p == UI_STYLE_BORDER_COLOR ||
           p == UI_STYLE_CORNER_RADIUS || p == UI_STYLE_OPACITY || p == UI_STYLE_TEXT_COLOR;
}

bool ui_style_prop_known(uint16_t prop) {
    if (prop & 0x8000u) return transitionable(prop & 0x7FFFu);
    return prop >= UI_STYLE_BACKGROUND_COLOR && prop <= UI_STYLE_MAX_LINES;
}

static bool color_ok(const float *c) {
    return isfinite(c[0]) && isfinite(c[1]) && isfinite(c[2]) && isfinite(c[3]);
}

bool ui_style_value_valid(uint16_t prop, const ui_value *v, bool override) {
    if (!ui_style_prop_known(prop)) return false;
    if (v->kind == UI_VALUE_UNSET) return override;
    if (prop & 0x8000u) {
        if (v->kind == UI_VALUE_NONE) return true;
        return v->kind == UI_VALUE_SPRING && isfinite(v->spring.stiffness) &&
               v->spring.stiffness > 0 && isfinite(v->spring.damping) && v->spring.damping >= 0;
    }
    if (v->kind == UI_VALUE_TOKEN) return v->token != 0;
    float n = v->number;
    switch ((ui_style_prop)prop) {
    case UI_STYLE_BACKGROUND_COLOR:
    case UI_STYLE_BORDER_COLOR:
    case UI_STYLE_TEXT_COLOR: return v->kind == UI_VALUE_COLOR && color_ok(v->color);
    case UI_STYLE_BORDER_WIDTH:
    case UI_STYLE_CORNER_RADIUS: return v->kind == UI_VALUE_PX && isfinite(n) && n >= 0;
    case UI_STYLE_OPACITY: return v->kind == UI_VALUE_NUMBER && n >= 0 && n <= 1;
    case UI_STYLE_CLIP: return v->kind == UI_VALUE_KEYWORD && v->keyword <= UI_CLIP_RECT;
    case UI_STYLE_LINE_HEIGHT:
        return v->kind == UI_VALUE_AUTO || (v->kind == UI_VALUE_NUMBER && n > 0 && n <= 100);
    case UI_STYLE_TEXT_ALIGN: return v->kind == UI_VALUE_KEYWORD && v->keyword <= UI_TEXT_ALIGN_JUSTIFY;
    case UI_STYLE_MAX_LINES:
        return v->kind == UI_VALUE_NONE ||
               (v->kind == UI_VALUE_NUMBER && n >= 0 && n <= 65535 && n == floorf(n));
    }
    return false;
}

bool ui_style_token_value_valid(const ui_value *v) {
    switch (v->kind) {
    case UI_VALUE_PX:
    case UI_VALUE_PERCENT:
    case UI_VALUE_NUMBER: return isfinite(v->number);
    case UI_VALUE_COLOR: return color_ok(v->color);
    case UI_VALUE_KEYWORD:
    case UI_VALUE_AUTO:
    case UI_VALUE_NONE: return true;
    case UI_VALUE_TOKEN: return v->token != 0;
    default: return false;
    }
}
