// The layout fixtures, generated from tools/layout-fixtures (see gen.py there).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ui/ui.h"

typedef struct layout_fixture_node {
    int32_t parent;              // index of the parent node, -1 for the surface
    const ui_style_decl *decls;  // its inline style
    uint32_t decl_count;
    const char *text;            // a text leaf's text, else NULL
    float font_size;             // px
    float expected[3][4];        // Chrome's border box, relative to the parent's, at each scale
} layout_fixture_node;

// The device scales fixtures are recorded at (expected[] and expected_failure[] follow it).
#define LAYOUT_FIXTURE_SCALES 3
static const float layout_fixture_scales[LAYOUT_FIXTURE_SCALES] = {1.f, 1.25f, 1.5f};

typedef struct layout_fixture {
    const char *name;
    uint32_t width, height;      // the surface, logical px
    const layout_fixture_node *nodes;
    uint32_t node_count;
    bool has_text;               // compared with the text tolerance
    bool expected_failure[3];    // listed in tools/layout-fixtures/deviations.md, per scale
} layout_fixture;

extern const layout_fixture layout_fixtures[];
extern const uint32_t layout_fixture_count;
