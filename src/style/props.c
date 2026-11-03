#include "style/props.h"

#include <math.h>
#include <string.h>

#include "unicode/utf8.h"

uint32_t ui_style_kind_words(uint8_t kind) {
    switch (kind) {
    case UI_VALUE_UNSET:
    case UI_VALUE_AUTO:
    case UI_VALUE_NONE:
    case UI_VALUE_INHERIT:
    case UI_VALUE_INITIAL: return 1;
    case UI_VALUE_STRING: return 2; // u16 length, then the bytes: at least one more word
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

uint32_t ui_style_value_words(const ui_value *v) {
    if (v->kind == UI_VALUE_STRING) return 1 + (2 + v->string.length + 3) / 4;
    return ui_style_kind_words((uint8_t)v->kind);
}

static bool transitionable(uint16_t p) {
    return p == UI_STYLE_BACKGROUND_COLOR || p == UI_STYLE_BORDER_COLOR ||
           p == UI_STYLE_CORNER_RADIUS || p == UI_STYLE_OPACITY || p == UI_STYLE_TEXT_COLOR ||
           p == UI_STYLE_LAYOUT;
}

bool ui_style_prop_known(uint16_t prop) {
    if (prop & 0x8000u) return transitionable(prop & 0x7FFFu);
    return (prop >= UI_STYLE_BACKGROUND_COLOR && prop <= UI_STYLE_DIRECTION) ||
           (prop >= UI_STYLE_DISPLAY && prop <= UI_STYLE_ASPECT_RATIO);
}

static bool length(const ui_value *v, bool negative, bool automatic, bool none) {
    if (v->kind == UI_VALUE_AUTO) return automatic;
    if (v->kind == UI_VALUE_NONE) return none;
    return (v->kind == UI_VALUE_PX || v->kind == UI_VALUE_PERCENT) && isfinite(v->number) &&
           (negative || v->number >= 0);
}

static bool keyword_in(const ui_value *v, uint32_t mask) {
    return v->kind == UI_VALUE_KEYWORD && v->keyword < 32 && (mask >> v->keyword & 1u);
}

static bool color_ok(const float *c) {
    return isfinite(c[0]) && isfinite(c[1]) && isfinite(c[2]) && isfinite(c[3]);
}

bool ui_style_string_valid(const ui_value *v) {
    return v->kind == UI_VALUE_STRING && v->string.length && v->string.length <= UI_STYLE_STRING_MAX &&
           v->string.bytes && !memchr(v->string.bytes, 0, v->string.length) &&
           ui_utf8_valid((const uint8_t *)v->string.bytes, v->string.length);
}

bool ui_style_value_valid(uint16_t prop, const ui_value *v, bool override) {
    if (!ui_style_prop_known(prop)) return false;
    if (v->kind == UI_VALUE_UNSET) return override;
    // Every declarable property and transition takes inherit and initial (CSS-wide keywords).
    if (v->kind == UI_VALUE_INHERIT || v->kind == UI_VALUE_INITIAL) return prop != UI_STYLE_LAYOUT;
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
    case UI_STYLE_FONT_FAMILY: return ui_style_string_valid(v);
    case UI_STYLE_FONT_SIZE: return v->kind == UI_VALUE_PX && n > 0 && n <= 4096;
    case UI_STYLE_FONT_WEIGHT: return v->kind == UI_VALUE_NUMBER && n >= 1 && n <= 1000 && n == floorf(n);
    case UI_STYLE_FONT_SLANT: return v->kind == UI_VALUE_KEYWORD && v->keyword <= UI_TEXT_OBLIQUE;
    case UI_STYLE_LANGUAGE:
        return v->kind == UI_VALUE_NONE || (ui_style_string_valid(v) && v->string.length <= 64);
    case UI_STYLE_DIRECTION: return v->kind == UI_VALUE_KEYWORD && v->keyword <= UI_DIRECTION_RTL;
    case UI_STYLE_DISPLAY: return keyword_in(v, 0x3);
    case UI_STYLE_POSITION: return keyword_in(v, 0x3);
    case UI_STYLE_FLEX_DIRECTION: return keyword_in(v, 0xF);
    case UI_STYLE_FLEX_WRAP: return keyword_in(v, 0x7);
    case UI_STYLE_JUSTIFY_CONTENT: return keyword_in(v, 0x1CF);  // normal start end center space-*
    case UI_STYLE_ALIGN_ITEMS:
    case UI_STYLE_ALIGN_SELF: return keyword_in(v, 0x3F);        // normal/auto .. stretch
    case UI_STYLE_ALIGN_CONTENT: return keyword_in(v, 0x1EF);    // all but baseline
    case UI_STYLE_FLEX_GROW:
    case UI_STYLE_FLEX_SHRINK: return v->kind == UI_VALUE_NUMBER && isfinite(n) && n >= 0;
    case UI_STYLE_FLEX_BASIS:
    case UI_STYLE_WIDTH:
    case UI_STYLE_HEIGHT:
    case UI_STYLE_MIN_WIDTH:
    case UI_STYLE_MIN_HEIGHT: return length(v, false, true, false);
    case UI_STYLE_MAX_WIDTH:
    case UI_STYLE_MAX_HEIGHT: return length(v, false, false, true);
    case UI_STYLE_ROW_GAP:
    case UI_STYLE_COLUMN_GAP:
    case UI_STYLE_PADDING_TOP:
    case UI_STYLE_PADDING_RIGHT:
    case UI_STYLE_PADDING_BOTTOM:
    case UI_STYLE_PADDING_LEFT: return length(v, false, false, false);
    case UI_STYLE_MARGIN_TOP:
    case UI_STYLE_MARGIN_RIGHT:
    case UI_STYLE_MARGIN_BOTTOM:
    case UI_STYLE_MARGIN_LEFT:
    case UI_STYLE_TOP:
    case UI_STYLE_RIGHT:
    case UI_STYLE_BOTTOM:
    case UI_STYLE_LEFT: return length(v, true, true, false);
    case UI_STYLE_ASPECT_RATIO:
        return v->kind == UI_VALUE_AUTO || (v->kind == UI_VALUE_NUMBER && isfinite(n) && n > 0);
    case UI_STYLE_LAYOUT: return false; // only its transition (unknown above)
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
    case UI_VALUE_STRING: return ui_style_string_valid(v);
    default: return false; // unset, spring, inherit, initial: not a token's value
    }
}
