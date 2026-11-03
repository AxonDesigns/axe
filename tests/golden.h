// Golden-image checks shared by the golden tests. A case renders under both schedulers;
// the two images must be byte-identical, and must match the stored golden
// (tests/goldens/<name>.qoi, generated under lavapipe) within a small tolerance that absorbs
// sRGB blending differences between driver versions: at most GOLDEN_MAX_BAD of the pixels
// may differ by more than GOLDEN_CHANNEL_TOL in a channel.
//
// UI_UPDATE_GOLDENS=1 rewrites the goldens. On a mismatch the actual image, the golden and
// a diff are written as PNGs next to the test binary (UI_TEST_OUT).
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image/image_codec.h"
#include "runtime/common.h"
#include "test.h"
#include "ui/ui.h"

#define GOLDEN_CHANNEL_TOL 2
#define GOLDEN_MAX_BAD 0.0005 // fraction of pixels

static void golden_write_diff(const ui_rgba_image *a, const ui_rgba_image *b, const char *path) {
    ui_rgba_image d = {a->width, a->height, malloc((size_t)a->width * a->height * 4)};
    if (!d.pixels) return;
    for (size_t i = 0; i < (size_t)a->width * a->height; i++) {
        int m = 0;
        for (int k = 0; k < 3; k++) {
            int v = abs(a->pixels[i * 4 + k] - b->pixels[i * 4 + k]);
            if (v > m) m = v;
        }
        uint8_t v = (uint8_t)(m > GOLDEN_CHANNEL_TOL ? 255 : m * 60);
        d.pixels[i * 4] = v;
        d.pixels[i * 4 + 1] = (uint8_t)(m > GOLDEN_CHANNEL_TOL ? 0 : v);
        d.pixels[i * 4 + 2] = (uint8_t)(m > GOLDEN_CHANNEL_TOL ? 0 : v);
        d.pixels[i * 4 + 3] = 255;
    }
    ui_png_write(&d, path);
    free(d.pixels);
}

// Checks one case: `name` names the golden file (for instance "latin@125").
static void golden_check(const char *name, const ui_image *single, const ui_image *threaded) {
    CHECK(single->width == threaded->width && single->height == threaded->height);
    if (single->height != threaded->height ||
        memcmp(single->pixels, threaded->pixels, (size_t)single->stride * single->height)) {
        fprintf(stderr, "  %s: schedulers differ\n", name);
        test_failures++;
    }
    ui_rgba_image actual = {single->width, single->height, single->pixels};
    char golden[512], out[512];
    snprintf(golden, sizeof golden, "%s/%s.qoi", UI_TEST_GOLDENS, name);
    if (ui_env_flag("UI_UPDATE_GOLDENS")) {
        uint8_t *q;
        size_t qn;
        CHECK(ui_qoi_encode(&actual, &q, &qn) == UI_OK);
        CHECK(ui_file_write(golden, q, qn) == UI_OK);
        free(q);
        fprintf(stderr, "  wrote %s\n", golden);
        return;
    }
    uint8_t *data;
    size_t size;
    ui_rgba_image expected = {0};
    if (ui_file_read(golden, &data, &size) != UI_OK) {
        fprintf(stderr, "  missing golden %s (run with UI_UPDATE_GOLDENS=1)\n", golden);
        test_failures++;
        return;
    }
    ui_result dr = ui_qoi_decode(data, size, &expected);
    free(data);
    CHECK(dr == UI_OK);
    if (dr != UI_OK) return;
    bool same_size = expected.width == actual.width && expected.height == actual.height;
    size_t bad = 0, pixels = (size_t)actual.width * actual.height;
    for (size_t i = 0; same_size && i < pixels; i++) {
        for (int k = 0; k < 4; k++) {
            if (abs(actual.pixels[i * 4 + k] - expected.pixels[i * 4 + k]) > GOLDEN_CHANNEL_TOL) {
                bad++;
                break;
            }
        }
    }
    if (!same_size || (double)bad > GOLDEN_MAX_BAD * (double)pixels) {
        fprintf(stderr, "  %s: %zu of %zu pixels differ from the golden%s\n", name, bad, pixels,
                same_size ? "" : " (size differs)");
        test_failures++;
        snprintf(out, sizeof out, "%s/%s-actual.png", UI_TEST_OUT, name);
        ui_png_write(&actual, out);
        snprintf(out, sizeof out, "%s/%s-expected.png", UI_TEST_OUT, name);
        ui_png_write(&expected, out);
        if (same_size) {
            snprintf(out, sizeof out, "%s/%s-diff.png", UI_TEST_OUT, name);
            golden_write_diff(&actual, &expected, out);
        }
    }
    free(expected.pixels);
}
