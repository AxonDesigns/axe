// The coverage rasterizer (exact area on analytic shapes, winding, subpixel offsets) and the
// glyph atlas (caching, updates, packing, eviction rules, page growth and release).
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "text/atlas.h"

#define FONT(name) UI_TEST_FONTS "/" name

static ui_logger quiet = {UI_LOG_WARN};

static double total_coverage(const ui_bitmap *bm) {
    double sum = 0;
    for (uint32_t i = 0; i < bm->width * bm->height; i++) sum += bm->pixels[i] / 255.0;
    return sum;
}

static uint8_t pixel_at(const ui_bitmap *bm, int32_t x, int32_t y) {
    // x, y in the shape's pixel grid (y down from the bitmap origin row)
    int32_t bx = x - bm->left, by = y - bm->top;
    if (bx < 0 || by < 0 || bx >= (int32_t)bm->width || by >= (int32_t)bm->height) return 0;
    return bm->pixels[by * (int32_t)bm->width + bx];
}

static void rect(ui_path_sink *s, float x0, float y0, float x1, float y1, bool ccw) {
    s->move_to(s->user, x0, y0);
    if (ccw) {
        s->line_to(s->user, x1, y0);
        s->line_to(s->user, x1, y1);
        s->line_to(s->user, x0, y1);
    } else {
        s->line_to(s->user, x0, y1);
        s->line_to(s->user, x1, y1);
        s->line_to(s->user, x1, y0);
    }
    s->close(s->user);
}

static void test_square(void) {
    ui_raster r = {0};
    ui_bitmap bm;
    // A 10x10 square at fractional offsets: total coverage is its area, interior pixels are
    // full and edge pixels carry exactly the covered fraction.
    for (int k = 0; k < 4; k++) {
        float off = 0.25f * (float)k;
        ui_raster_begin(&r);
        ui_path_sink s = ui_raster_sink(&r);
        rect(&s, 2, 2, 12, 12, true);
        CHECK(ui_raster_end(&r, off, off, &bm) == UI_OK);
        // Exact up to 8-bit quantization: at most 0.5/255 on each of ~44 edge pixels.
        CHECK_NEAR(total_coverage(&bm), 100.0, 0.1);
        // y up in path space: rows -12..-2 after the flip, shifted by the offset.
        CHECK_EQ_INT(pixel_at(&bm, 5, -8), 255);
        if (k == 1) {
            CHECK_NEAR(pixel_at(&bm, 2, -8), 0.75 * 255, 1);  // left edge at x = 2.25
            CHECK_NEAR(pixel_at(&bm, 12, -8), 0.25 * 255, 1); // right edge at x = 12.25
        }
    }
    // Same-direction overlap counts once; an opposite-direction inner square is a hole.
    ui_raster_begin(&r);
    ui_path_sink s = ui_raster_sink(&r);
    rect(&s, 0, 0, 10, 10, true);
    rect(&s, 5, 0, 15, 10, true);
    CHECK(ui_raster_end(&r, 0, 0, &bm) == UI_OK);
    CHECK_NEAR(total_coverage(&bm), 150.0, 0.1);
    ui_raster_begin(&r);
    s = ui_raster_sink(&r);
    rect(&s, 0, 0, 10, 10, true);
    rect(&s, 3, 3, 7, 7, false);
    CHECK(ui_raster_end(&r, 0, 0, &bm) == UI_OK);
    CHECK_NEAR(total_coverage(&bm), 84.0, 0.1);
    CHECK_EQ_INT(pixel_at(&bm, 5, -5), 0);
    // Empty path.
    ui_raster_begin(&r);
    CHECK(ui_raster_end(&r, 0, 0, &bm) == UI_OK);
    CHECK_EQ_INT(bm.width, 0);
    ui_raster_free(&r);
}

static void test_circle(void) {
    ui_raster r = {0};
    ui_bitmap bm;
    // Four cubic quarter arcs (kappa) and the same circle from quadratics: within 0.5% of
    // pi r^2 (the cubic approximation itself is off by ~0.03%).
    const float R = 20, c = 30, k = 0.5522847498f * R;
    ui_raster_begin(&r);
    ui_path_sink s = ui_raster_sink(&r);
    s.move_to(s.user, c + R, c);
    s.cubic_to(s.user, c + R, c + k, c + k, c + R, c, c + R);
    s.cubic_to(s.user, c - k, c + R, c - R, c + k, c - R, c);
    s.cubic_to(s.user, c - R, c - k, c - k, c - R, c, c - R);
    s.cubic_to(s.user, c + k, c - R, c + R, c - k, c + R, c);
    s.close(s.user);
    CHECK(ui_raster_end(&r, 0.3f, 0, &bm) == UI_OK);
    double area = M_PI * R * R;
    CHECK(fabs(total_coverage(&bm) - area) / area < 0.005);

    ui_raster_begin(&r);
    s = ui_raster_sink(&r);
    const int n = 16;
    s.move_to(s.user, c + R, c);
    for (int i = 0; i < n; i++) {
        double a0 = 2 * M_PI * i / n, a1 = 2 * M_PI * (i + 1) / n, am = (a0 + a1) / 2;
        double rc = R / cos(M_PI / n); // control point on the tangent intersection
        s.quad_to(s.user, (float)(c + rc * cos(am)), (float)(c + rc * sin(am)),
                  (float)(c + R * cos(a1)), (float)(c + R * sin(a1)));
    }
    s.close(s.user);
    CHECK(ui_raster_end(&r, 0, 0.6f, &bm) == UI_OK);
    CHECK(fabs(total_coverage(&bm) - area) / area < 0.005);
    ui_raster_free(&r);
}

static void test_glyph(void) {
    ui_face *face;
    CHECK(ui_face_load_file(FONT("NotoSans-VF.ttf"), 0, &face) == UI_OK);
    ui_font *font;
    CHECK(ui_font_create(face, 48, NULL, 0, &font) == UI_OK);
    ui_raster r = {0};
    ui_bitmap bm;
    uint32_t o = ui_font_glyph(font, 'o');
    CHECK(ui_raster_glyph(&r, font, o, 0, &bm) == UI_OK);
    CHECK(bm.width > 20 && bm.height > 20);
    CHECK(bm.top < 0 && bm.top + (int32_t)bm.height >= 0); // sits on the baseline
    // The counter is empty, the stroke is solid.
    uint32_t cx = bm.width / 2, cy = bm.height / 2;
    CHECK_EQ_INT(bm.pixels[cy * bm.width + cx], 0);
    CHECK_EQ_INT(bm.pixels[cy * bm.width + 2], 255);
    double full = total_coverage(&bm);
    // A subpixel shift moves ink but keeps its amount.
    CHECK(ui_raster_glyph(&r, font, o, 0.5f, &bm) == UI_OK);
    CHECK(fabs(total_coverage(&bm) - full) / full < 0.002);
    // Variations change the outline: bold has more ink.
    ui_font *bold;
    ui_font_axis wght = {UI_AXIS_WGHT, 900};
    CHECK(ui_font_create(face, 48, &wght, 1, &bold) == UI_OK);
    CHECK(ui_raster_glyph(&r, bold, o, 0, &bm) == UI_OK);
    CHECK(total_coverage(&bm) > full * 1.3);
    // A space has no outline.
    CHECK(ui_raster_glyph(&r, font, ui_font_glyph(font, ' '), 0, &bm) == UI_OK);
    CHECK_EQ_INT(bm.width, 0);
    ui_raster_free(&r);
    ui_font_destroy(bold);
    ui_font_destroy(font);
    ui_face_destroy(face);
}

typedef struct fixture {
    ui_font_registry *fonts;
    ui_font *font;
    ui_font_key key;
    ui_raster raster;
} fixture;

static void fixture_init(fixture *f, float size) {
    memset(f, 0, sizeof *f);
    ui_font_registry_desc desc = {.log = &quiet};
    CHECK(ui_font_registry_create(&desc, &f->fonts) == UI_OK);
    CHECK(ui_font_registry_add_file(f->fonts, FONT("NotoSans-VF.ttf"), 0, NULL) == UI_OK);
    f->font = ui_font_registry_font(f->fonts, 0, size, 400, UI_FONT_NORMAL, &f->key);
}

static void fixture_free(fixture *f) {
    ui_raster_free(&f->raster);
    ui_font_registry_destroy(f->fonts);
}

static const ui_atlas_glyph *get(ui_atlas *a, fixture *f, uint32_t cp, uint32_t bin) {
    ui_glyph_key key = {f->key, ui_font_glyph(f->font, cp), bin};
    return ui_atlas_get(a, &f->raster, f->font, key);
}

static void test_atlas_cache(void) {
    fixture f;
    fixture_init(&f, 24);
    ui_atlas *a;
    CHECK(ui_atlas_create(&(ui_atlas_desc){0}, &a) == UI_OK);
    ui_atlas_begin_frame(a, 1, UINT64_MAX);
    const ui_atlas_glyph *g = get(a, &f, 'A', 0);
    CHECK(g && g->width > 0 && g->height > 0);
    ui_atlas_glyph first = *g;
    CHECK(get(a, &f, 'A', 0)->x == first.x);
    const ui_atlas_glyph *b1 = get(a, &f, 'A', 1);
    CHECK(b1 && (b1->x != first.x || b1->y != first.y)); // another bin is another glyph
    const ui_atlas_glyph *sp = get(a, &f, ' ', 0);
    CHECK(sp && sp->width == 0);
    ui_atlas_stats st;
    ui_atlas_get_stats(a, &st);
    CHECK_EQ_INT(st.rasterized, 3);
    CHECK_EQ_INT(st.glyphs, 3);
    CHECK_EQ_INT(st.pages, 1);

    bool failed;
    ui_atlas_update *u = ui_atlas_take_update(a, &failed);
    CHECK(u && !failed);
    CHECK_EQ_INT(u->seq, 1);
    CHECK_EQ_INT(u->page_op_count, 1);
    CHECK_EQ_INT(u->page_ops[0].size, 1024);
    CHECK_EQ_INT(u->write_count, 2); // the space needs no texels
    // The written texels are the rasterized glyph.
    ui_bitmap bm;
    CHECK(ui_raster_glyph(&f.raster, f.font, ui_font_glyph(f.font, 'A'), 0, &bm) == UI_OK);
    CHECK_EQ_INT(u->writes[0].width, bm.width);
    CHECK(!memcmp(u->pixels + u->writes[0].offset, bm.pixels, bm.width * bm.height));
    free(u);
    CHECK(ui_atlas_take_update(a, &failed) == NULL && !failed);
    CHECK_EQ_INT(ui_atlas_seq(a), 1);
    ui_atlas_end_frame(a);
    ui_atlas_destroy(a);
    fixture_free(&f);
}

static void test_atlas_eviction(void) {
    fixture f;
    fixture_init(&f, 24);
    ui_atlas *a;
    CHECK(ui_atlas_create(&(ui_atlas_desc){.evict_frames = 3}, &a) == UI_OK);
    ui_atlas_begin_frame(a, 1, UINT64_MAX);
    CHECK(get(a, &f, 'A', 0));
    CHECK(get(a, &f, 'B', 0));
    ui_atlas_end_frame(a);
    // 'B' stays in use; 'A' goes unused.
    for (uint64_t e = 2; e <= 3; e++) {
        ui_atlas_begin_frame(a, e, UINT64_MAX);
        get(a, &f, 'B', 0);
        ui_atlas_end_frame(a);
    }
    ui_atlas_stats st;
    ui_atlas_get_stats(a, &st);
    CHECK_EQ_INT(st.evicted, 0);
    ui_atlas_begin_frame(a, 4, UINT64_MAX);
    get(a, &f, 'B', 0);
    ui_atlas_end_frame(a);
    ui_atlas_get_stats(a, &st);
    CHECK_EQ_INT(st.evicted, 1); // 'A' after 3 unused epochs
    CHECK_EQ_INT(st.glyphs, 1);

    // A glyph that an unfinished display list may still draw is never evicted.
    ui_atlas_begin_frame(a, 5, UINT64_MAX);
    get(a, &f, 'C', 0);
    ui_atlas_end_frame(a);
    for (uint64_t e = 6; e < 20; e++) {
        ui_atlas_begin_frame(a, e, 5); // a surface's newest unfinished list is from epoch 5
        ui_atlas_end_frame(a);
    }
    // 'B' (last used at epoch 4, before the protected list) went; 'C' stays.
    ui_atlas_get_stats(a, &st);
    CHECK_EQ_INT(st.glyphs, 1);
    ui_atlas_begin_frame(a, 20, UINT64_MAX);
    ui_atlas_end_frame(a);
    ui_atlas_get_stats(a, &st);
    CHECK_EQ_INT(st.glyphs, 0);
    // Re-requesting an evicted glyph rasterizes it again.
    ui_atlas_begin_frame(a, 21, UINT64_MAX);
    uint64_t before = st.rasterized;
    CHECK(get(a, &f, 'A', 0));
    ui_atlas_get_stats(a, &st);
    CHECK_EQ_INT(st.rasterized, before + 1);
    ui_atlas_destroy(a);
    fixture_free(&f);
}

// 'H' at 30 px plus i/64 px: glyphs of nearly equal size with distinct cache keys.
static const ui_atlas_glyph *get_h_bin(ui_atlas *a, fixture *f, uint32_t i, uint32_t bin) {
    ui_font_key key;
    ui_font *font = ui_font_registry_font(f->fonts, 0, 30.f + (float)i / 64.f, 400,
                                          UI_FONT_NORMAL, &key);
    return ui_atlas_get(a, &f->raster, font, (ui_glyph_key){key, ui_font_glyph(font, 'H'), bin});
}
static const ui_atlas_glyph *get_h(ui_atlas *a, fixture *f, uint32_t i) {
    return get_h_bin(a, f, i, 0);
}

static void test_atlas_pressure_and_pages(void) {
    fixture f;
    fixture_init(&f, 30);
    ui_atlas *a;
    // One tiny page, filled within one frame: nothing used this frame may be evicted.
    CHECK(ui_atlas_create(&(ui_atlas_desc){.page_size = 128, .max_pages = 1, .evict_frames = 2},
                          &a) == UI_OK);
    ui_atlas_begin_frame(a, 1, UINT64_MAX);
    uint32_t i = 0;
    while (i < 500 && get_h(a, &f, i)->width) i++;
    CHECK(i > 4 && i < 500);
    ui_atlas_stats st;
    ui_atlas_get_stats(a, &st);
    CHECK_EQ_INT(st.pages, 1);
    CHECK_EQ_INT(st.rejected, 1);
    CHECK_EQ_INT(st.evicted, 0);
    ui_atlas_end_frame(a);
    // Next frame: a new glyph may evict last frame's under pressure, unless protected.
    ui_atlas_begin_frame(a, 2, 1);
    CHECK_EQ_INT(get_h_bin(a, &f, 0, 1)->width, 0);
    ui_atlas_get_stats(a, &st);
    CHECK_EQ_INT(st.rejected, 2);
    ui_atlas_end_frame(a);
    // Unprotected, eviction goes least recently used first and stops once the glyph fits:
    // glyph 0 is used again this frame and survives, and only a batch of 8 goes.
    ui_atlas_begin_frame(a, 3, UINT64_MAX);
    CHECK(get_h(a, &f, 0)->width > 0);
    CHECK(get_h_bin(a, &f, 1, 1)->width > 0);
    ui_atlas_get_stats(a, &st);
    CHECK_EQ_INT(st.evicted, 8);
    uint64_t rasterized = st.rasterized;
    CHECK(get_h(a, &f, 0)->width > 0);
    ui_atlas_get_stats(a, &st);
    CHECK_EQ_INT(st.rasterized, rasterized); // still cached
    ui_atlas_destroy(a);

    // A glyph may fill most of a page (it used to be limited to a quarter page).
    CHECK(ui_atlas_create(&(ui_atlas_desc){.page_size = 128, .max_pages = 1}, &a) == UI_OK);
    ui_atlas_begin_frame(a, 1, UINT64_MAX);
    ui_font_key big_key;
    ui_font *big = ui_font_registry_font(f.fonts, 0, 120, 400, UI_FONT_NORMAL, &big_key);
    const ui_atlas_glyph *bg = ui_atlas_get(a, &f.raster, big,
                                            (ui_glyph_key){big_key, ui_font_glyph(big, 'H'), 0});
    CHECK(bg && bg->height > 64 && bg->height <= 128);
    ui_atlas_get_stats(a, &st);
    CHECK_EQ_INT(st.rejected, 0);
    ui_atlas_destroy(a);

    // Two pages: the atlas grows to the second, and releases it once it stays empty.
    CHECK(ui_atlas_create(&(ui_atlas_desc){.page_size = 128, .max_pages = 2, .evict_frames = 2},
                          &a) == UI_OK);
    ui_atlas_begin_frame(a, 1, UINT64_MAX);
    for (i = 0; i < 500; i++) {
        get_h(a, &f, i);
        ui_atlas_get_stats(a, &st);
        if (st.pages == 2) break;
    }
    CHECK_EQ_INT(st.pages, 2);
    ui_atlas_end_frame(a);
    bool failed;
    ui_atlas_update *u = ui_atlas_take_update(a, &failed);
    CHECK(u && u->page_op_count == 2 && u->page_ops[1].page == 1 && u->page_ops[1].size == 128);
    free(u);
    for (uint64_t e = 2; e < 10; e++) {
        ui_atlas_begin_frame(a, e, UINT64_MAX);
        ui_atlas_end_frame(a);
    }
    ui_atlas_get_stats(a, &st);
    CHECK_EQ_INT(st.glyphs, 0);
    CHECK_EQ_INT(st.pages, 1);
    u = ui_atlas_take_update(a, &failed);
    CHECK(u && u->page_op_count == 1 && u->page_ops[0].page == 1 && u->page_ops[0].size == 0);
    free(u);
    // Freed space is reused on the remaining page.
    ui_atlas_begin_frame(a, 10, UINT64_MAX);
    for (i = 0; i < 4; i++) CHECK(get_h(a, &f, i)->width > 0);
    ui_atlas_get_stats(a, &st);
    CHECK_EQ_INT(st.pages, 1);
    ui_atlas_destroy(a);
    fixture_free(&f);
}

static void test_atlas_many(void) {
    // Thousands of glyphs at several sizes: the table grows, nothing overlaps.
    fixture f;
    fixture_init(&f, 12);
    ui_atlas *a;
    CHECK(ui_atlas_create(&(ui_atlas_desc){0}, &a) == UI_OK);
    ui_atlas_begin_frame(a, 1, UINT64_MAX);
    uint32_t n = 0;
    ui_atlas_glyph *placed = malloc(4000 * sizeof *placed);
    for (uint32_t gid = 1; gid < 1000; gid++) {
        for (uint32_t bin = 0; bin < 4; bin++) {
            const ui_atlas_glyph *g = ui_atlas_get(a, &f.raster, f.font,
                                                   (ui_glyph_key){f.key, gid, bin});
            CHECK(g != NULL);
            if (g && g->width) placed[n++] = *g;
        }
    }
    bool overlap = false;
    for (uint32_t i = 0; i < n && !overlap; i++) {
        for (uint32_t j = i + 1; j < n && !overlap; j++) {
            const ui_atlas_glyph *p = &placed[i], *q = &placed[j];
            overlap = p->page == q->page && p->x < q->x + q->width && q->x < p->x + p->width &&
                      p->y < q->y + q->height && q->y < p->y + p->height;
        }
    }
    CHECK(!overlap);
    CHECK(n > 3000);
    free(placed);
    ui_atlas_destroy(a);
    fixture_free(&f);
}

int main(void) {
    RUN(test_square);
    RUN(test_circle);
    RUN(test_glyph);
    RUN(test_atlas_cache);
    RUN(test_atlas_eviction);
    RUN(test_atlas_pressure_and_pages);
    RUN(test_atlas_many);
    return test_failures != 0;
}
