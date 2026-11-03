// Layout against Chrome: every fixture in tools/layout-fixtures is built through the wire,
// laid out at scale 1 with the same font, and each node's border box is compared with the
// one Chrome 156 recorded. Boxes without text must match within 1/64 px (Chrome's
// LayoutUnit), boxes in fixtures with text within 0.5 px (text measures in whole-pixel
// line boxes and HarfBuzz advances on both sides, rounded differently). Fixtures listed in
// tools/layout-fixtures/deviations.md are expected to fail; one that passes fails the test.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/scene.h"
#include "layout/layout_fixtures.h"
#include "test.h"

#define BOX_TOLERANCE (1.0 / 64 + 1e-4)
#define TEXT_TOLERANCE 0.5

static ui_logger quiet = {UI_LOG_ERROR};
static ui_font_registry *fonts;
static ui_text_context text;

// Lays out one fixture; returns the worst difference from Chrome (and where it is).
static double run(const layout_fixture *f, int scale, uint32_t *worst_node, bool verbose) {
    ui_scene s;
    ui_scene_init(&s, &quiet);
    ui_handle surface = {(uint32_t)UI_HANDLE_SURFACE << UI_HANDLE_SLOT_BITS, 1};
    ui_scene_add_root(&s, surface);
    ui_cmd_writer *w;
    ui_cmd_writer_create(&w);
    static const char *const family[] = {"Noto Sans"};
    for (uint32_t i = 0; i < f->node_count; i++) {
        const layout_fixture_node *n = &f->nodes[i];
        ui_handle h = {i + 1, 1}, parent = n->parent < 0 ? surface : (ui_handle){(uint32_t)n->parent + 1, 1};
        CHECK(ui_cmd_create_node(w, h, parent) == UI_OK);
        if (n->decl_count) CHECK(ui_cmd_set_props(w, h, n->decls, n->decl_count) == UI_OK);
        if (!n->text) continue;
        ui_text_desc t = {.set = UI_TEXT_SET_ALL & ~UI_TEXT_SET_DIRECTION, .text = n->text, .length = (uint32_t)strlen(n->text),
                          .style = {.family = 0, .weight = 400, .size = n->font_size, .color = {0, 0, 0, 1}},
                          .families = family, .family_count = 1, .line_height = 1.25f};
        CHECK(ui_cmd_set_text(w, h, &t) == UI_OK);
    }
    uint8_t *bytes;
    size_t size;
    ui_cmd_writer_take(w, &bytes, &size);
    CHECK(ui_scene_apply(&s, bytes, size, 0) == UI_OK);
    CHECK_EQ_INT(s.stats.dropped, 0);
    free(bytes);
    ui_scene_resolve(&s, 0);
    ui_scene_layout(&s, surface, (float)f->width, (float)f->height, layout_fixture_scales[scale], &text, 0);
    double worst = 0;
    for (uint32_t i = 0; i < f->node_count; i++) {
        const float *got = s.nodes[i + 1].layout.rect, *want = f->nodes[i].expected[scale];
        for (int k = 0; k < 4; k++) {
            double d = fabs((double)got[k] - (double)want[k]);
            if (!(d <= worst)) {
                worst = isnan(d) ? INFINITY : d;
                *worst_node = i;
            }
        }
        if (verbose)
            fprintf(stderr, "    node %u: got %g %g %g %g, Chrome %g %g %g %g\n", i, (double)got[0],
                    (double)got[1], (double)got[2], (double)got[3], (double)want[0], (double)want[1],
                    (double)want[2], (double)want[3]);
    }
    ui_cmd_writer_destroy(w);
    ui_scene_destroy(&s);
    return worst;
}

static void test_conformance(void) {
    const char *only = getenv("UI_LAYOUT_FIXTURE"); // prints every box of the named fixture
    const char *only_scale = getenv("UI_LAYOUT_SCALE"); // 100, 125 or 150: just that scale
    for (int k = 0; k < LAYOUT_FIXTURE_SCALES; k++) {
        int percent = (int)lroundf(layout_fixture_scales[k] * 100);
        if (only_scale && atoi(only_scale) != percent) continue;
        uint32_t passed = 0, expected_failures = 0, total = 0;
        for (uint32_t i = 0; i < layout_fixture_count; i++) {
            const layout_fixture *f = &layout_fixtures[i];
            if (only && strcmp(only, f->name)) continue;
            total++;
            uint32_t node = 0;
            if (only) fprintf(stderr, "  at %d%%:\n", percent);
            double worst = run(f, k, &node, only != NULL);
            bool ok = worst <= (f->has_text ? TEXT_TOLERANCE : BOX_TOLERANCE);
            passed += ok;
            if (f->expected_failure[k]) {
                expected_failures++;
                if (ok) {
                    fprintf(stderr, "  %s@%d passes but is listed in deviations.md\n", f->name, percent);
                    test_failures++;
                }
            } else if (!ok) {
                fprintf(stderr, "  %s@%d: node %u off by %.4f px\n", f->name, percent, node, worst);
                test_failures++;
            }
        }
        fprintf(stderr, "  at %.2fx: %u of %u fixtures match Chrome (%.1f%%); %u known deviations\n",
                (double)layout_fixture_scales[k], passed, total, 100.0 * passed / total, expected_failures);
    }
}

int main(void) {
    ui_font_registry_create(&(ui_font_registry_desc){.log = &quiet}, &fonts);
    ui_font_registry_add_file(fonts, UI_TEST_FONTS "/NotoSans-Regular-Static.ttf", 0, NULL);
    ui_text_context_init(&text, fonts);
    RUN(test_conformance);
    ui_text_context_free(&text);
    ui_font_registry_destroy(fonts);
    return test_failures != 0;
}
