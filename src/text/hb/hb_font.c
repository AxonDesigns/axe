#include "text/hb/hb_font.h"

#include <hb-ot.h>
#include <hb.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/common.h"
#include "unicode/ucd.h"
#include "unicode/utf8.h"

struct ui_face {
    hb_blob_t *blob;
    hb_face_t *face;
    hb_font_t *probe; // unscaled font for cmap lookups
    hb_draw_funcs_t *draw;
    ui_face_info info;
};

struct ui_font {
    ui_face *face;
    hb_font_t *font;
    float size;
};

struct ui_shaper {
    hb_buffer_t *buffer;
    ui_glyph_pos *glyphs;
    uint32_t capacity;
};

// ---- Faces -------------------------------------------------------------------------------

static void read_family(hb_face_t *face, char *out, size_t cap) {
    unsigned len = (unsigned)cap;
    out[0] = 0;
    if (hb_ot_name_get_utf8(face, HB_OT_NAME_ID_TYPOGRAPHIC_FAMILY, HB_LANGUAGE_INVALID, &len,
                            out) == 0) {
        len = (unsigned)cap;
        hb_ot_name_get_utf8(face, HB_OT_NAME_ID_FONT_FAMILY, HB_LANGUAGE_INVALID, &len, out);
    }
}

static void move_to(hb_draw_funcs_t *f, void *data, hb_draw_state_t *st, float x, float y,
                    void *u) {
    (void)f, (void)st, (void)u;
    const ui_path_sink *s = data;
    s->move_to(s->user, x / 64.f, y / 64.f);
}
static void line_to(hb_draw_funcs_t *f, void *data, hb_draw_state_t *st, float x, float y,
                    void *u) {
    (void)f, (void)st, (void)u;
    const ui_path_sink *s = data;
    s->line_to(s->user, x / 64.f, y / 64.f);
}
static void quad_to(hb_draw_funcs_t *f, void *data, hb_draw_state_t *st, float cx, float cy,
                    float x, float y, void *u) {
    (void)f, (void)st, (void)u;
    const ui_path_sink *s = data;
    s->quad_to(s->user, cx / 64.f, cy / 64.f, x / 64.f, y / 64.f);
}
static void cubic_to(hb_draw_funcs_t *f, void *data, hb_draw_state_t *st, float c1x, float c1y,
                     float c2x, float c2y, float x, float y, void *u) {
    (void)f, (void)st, (void)u;
    const ui_path_sink *s = data;
    s->cubic_to(s->user, c1x / 64.f, c1y / 64.f, c2x / 64.f, c2y / 64.f, x / 64.f, y / 64.f);
}
static void close_path(hb_draw_funcs_t *f, void *data, hb_draw_state_t *st, void *u) {
    (void)f, (void)st, (void)u;
    const ui_path_sink *s = data;
    s->close(s->user);
}

static ui_result face_from_blob(hb_blob_t *blob, uint32_t index, ui_face **out) {
    hb_face_t *hface = hb_face_create_or_fail(blob, index);
    if (!hface) {
        hb_blob_destroy(blob);
        return UI_ERR_INVALID_ARGUMENT;
    }
    ui_face *f = calloc(1, sizeof *f);
    if (!f) {
        hb_face_destroy(hface);
        hb_blob_destroy(blob);
        return UI_ERR_OUT_OF_MEMORY;
    }
    f->blob = blob;
    f->face = hface;
    hb_face_make_immutable(hface);
    f->probe = hb_font_create(hface);
    f->draw = hb_draw_funcs_create();
    hb_draw_funcs_set_move_to_func(f->draw, move_to, NULL, NULL);
    hb_draw_funcs_set_line_to_func(f->draw, line_to, NULL, NULL);
    hb_draw_funcs_set_quadratic_to_func(f->draw, quad_to, NULL, NULL);
    hb_draw_funcs_set_cubic_to_func(f->draw, cubic_to, NULL, NULL);
    hb_draw_funcs_set_close_path_func(f->draw, close_path, NULL, NULL);
    hb_draw_funcs_make_immutable(f->draw);

    ui_face_info *info = &f->info;
    read_family(hface, info->family, sizeof info->family);
    info->upem = hb_face_get_upem(hface);
    float w = hb_style_get_value(f->probe, HB_STYLE_TAG_WEIGHT);
    info->weight = (uint16_t)fminf(fmaxf(roundf(w), 1.f), 1000.f);
    info->italic = hb_style_get_value(f->probe, HB_STYLE_TAG_ITALIC) > 0.5f ||
                   hb_style_get_value(f->probe, HB_STYLE_TAG_SLANT_ANGLE) != 0.f;
    hb_ot_var_axis_info_t axes[16];
    unsigned count = UI_ARRAY_LEN(axes);
    hb_ot_var_get_axis_infos(hface, 0, &count, axes);
    for (unsigned i = 0; i < count; i++) {
        if (axes[i].tag == HB_TAG('w', 'g', 'h', 't')) {
            info->has_wght = true;
            info->wght_min = axes[i].min_value;
            info->wght_max = axes[i].max_value;
        } else if (axes[i].tag == HB_TAG('i', 't', 'a', 'l')) {
            info->has_ital = true;
        } else if (axes[i].tag == HB_TAG('s', 'l', 'n', 't')) {
            info->has_slnt = true;
            info->slnt_min = axes[i].min_value;
        }
    }
    *out = f;
    return UI_OK;
}

ui_result ui_face_load_file(const char *path, uint32_t index, ui_face **out) {
    hb_blob_t *blob = hb_blob_create_from_file_or_fail(path);
    if (!blob) return UI_ERR_IO;
    return face_from_blob(blob, index, out);
}

ui_result ui_face_load_memory(const void *data, size_t size, uint32_t index, ui_face **out) {
    if (size > UINT32_MAX) return UI_ERR_INVALID_ARGUMENT;
    hb_blob_t *blob = hb_blob_create_or_fail(data, (unsigned)size, HB_MEMORY_MODE_DUPLICATE,
                                             NULL, NULL);
    if (!blob) return UI_ERR_OUT_OF_MEMORY;
    return face_from_blob(blob, index, out);
}

void ui_face_destroy(ui_face *f) {
    if (!f) return;
    hb_draw_funcs_destroy(f->draw);
    hb_font_destroy(f->probe);
    hb_face_destroy(f->face);
    hb_blob_destroy(f->blob);
    free(f);
}

const ui_face_info *ui_face_get_info(const ui_face *f) { return &f->info; }

bool ui_face_has_codepoint(const ui_face *f, uint32_t cp) {
    hb_codepoint_t g;
    return hb_font_get_nominal_glyph(f->probe, cp, &g);
}

// ---- Fonts -------------------------------------------------------------------------------

ui_result ui_font_create(ui_face *face, float size_px, const ui_font_axis *axes,
                         uint32_t axis_count, ui_font **out) {
    if (!(size_px > 0) || !isfinite(size_px)) return UI_ERR_INVALID_ARGUMENT;
    ui_font *f = calloc(1, sizeof *f);
    if (!f) return UI_ERR_OUT_OF_MEMORY;
    f->face = face;
    f->size = size_px;
    f->font = hb_font_create(face->face);
    int scale = (int)lroundf(size_px * 64.f);
    hb_font_set_scale(f->font, scale, scale);
    if (axis_count) {
        hb_variation_t vars[8];
        uint32_t n = axis_count < 8 ? axis_count : 8;
        for (uint32_t i = 0; i < n; i++) vars[i] = (hb_variation_t){axes[i].tag, axes[i].value};
        hb_font_set_variations(f->font, vars, n);
    }
    hb_font_make_immutable(f->font);
    if (hb_font_get_empty() == f->font) {
        free(f);
        return UI_ERR_OUT_OF_MEMORY;
    }
    *out = f;
    return UI_OK;
}

void ui_font_destroy(ui_font *f) {
    if (!f) return;
    hb_font_destroy(f->font);
    free(f);
}

ui_face *ui_font_face(const ui_font *f) { return f->face; }
float ui_font_size(const ui_font *f) { return f->size; }

void ui_font_get_metrics(const ui_font *f, ui_font_metrics *out) {
    hb_font_extents_t e;
    hb_font_get_h_extents(f->font, &e);
    out->ascent = (float)e.ascender / 64.f;
    out->descent = (float)-e.descender / 64.f;
    out->line_gap = (float)e.line_gap / 64.f;
}

uint32_t ui_font_glyph(const ui_font *f, uint32_t cp) {
    hb_codepoint_t g = 0;
    return hb_font_get_nominal_glyph(f->font, cp, &g) ? g : 0;
}

uint32_t ui_font_ligature_carets(const ui_font *f, uint32_t glyph, bool rtl, float *carets,
                                 uint32_t cap) {
    hb_position_t pos[16];
    unsigned n = cap < 16 ? cap : 16;
    unsigned total = hb_ot_layout_get_ligature_carets(
        f->font, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR, glyph, 0, &n, pos);
    for (unsigned i = 0; i < n; i++) carets[i] = (float)pos[i] / 64.f;
    return total;
}

// ---- Shaping -----------------------------------------------------------------------------

ui_result ui_shaper_create(ui_shaper **out) {
    ui_shaper *s = calloc(1, sizeof *s);
    if (!s) return UI_ERR_OUT_OF_MEMORY;
    s->buffer = hb_buffer_create();
    if (!hb_buffer_allocation_successful(s->buffer)) {
        hb_buffer_destroy(s->buffer);
        free(s);
        return UI_ERR_OUT_OF_MEMORY;
    }
    *out = s;
    return UI_OK;
}

void ui_shaper_destroy(ui_shaper *s) {
    if (!s) return;
    hb_buffer_destroy(s->buffer);
    free(s->glyphs);
    free(s);
}

static hb_codepoint_t space_glyph(const ui_font *font) {
    hb_codepoint_t g = 0;
    hb_font_get_nominal_glyph(font->font, ' ', &g);
    return g;
}

ui_result ui_shape(ui_shaper *s, ui_font *font, const ui_shape_request *req,
                   const ui_glyph_pos **glyphs, uint32_t *count) {
    hb_buffer_t *b = s->buffer;
    hb_buffer_clear_contents(b);
    // Clusters are byte offsets; the surrounding text gives the shaper context (joining
    // across run boundaries) without being shaped.
    hb_buffer_add_utf8(b, (const char *)req->text, (int)req->text_len, req->start, (int)req->len);
    hb_buffer_set_direction(b, req->rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    hb_buffer_set_script(b, hb_script_from_iso15924_tag(req->script));
    hb_buffer_set_language(b, hb_language_get_default());
    hb_buffer_set_cluster_level(b, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_GRAPHEMES);
    hb_buffer_set_flags(b, HB_BUFFER_FLAG_PRODUCE_UNSAFE_TO_CONCAT |
                               (req->start == 0 ? HB_BUFFER_FLAG_BOT : 0) |
                               (req->start + req->len == req->text_len ? HB_BUFFER_FLAG_EOT : 0));
    hb_shape(font->font, b, NULL, 0);
    if (!hb_buffer_allocation_successful(b)) return UI_ERR_OUT_OF_MEMORY;
    unsigned n;
    hb_glyph_info_t *info = hb_buffer_get_glyph_infos(b, &n);
    hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(b, NULL);
    if (ui_grow((void **)&s->glyphs, &s->capacity, n ? n : 1, sizeof *s->glyphs) != UI_OK)
        return UI_ERR_OUT_OF_MEMORY;
    for (unsigned i = 0; i < n; i++) {
        s->glyphs[i] = (ui_glyph_pos){
            .glyph = info[i].codepoint,
            .cluster = info[i].cluster,
            .advance = pos[i].x_advance,
            .x_offset = pos[i].x_offset,
            .y_offset = pos[i].y_offset,
            .unsafe_to_break = hb_glyph_info_get_glyph_flags(&info[i]) & HB_GLYPH_FLAG_UNSAFE_TO_BREAK,
        };
        size_t at = info[i].cluster;
        uint32_t cp = ui_utf8_next(req->text, &at);
        uint8_t gc = ui_ucd(cp)->gc;
        if (gc == UI_GC_CC || gc == UI_GC_ZL || gc == UI_GC_ZP) {
            ui_glyph_pos *g = &s->glyphs[i];
            g->glyph = UI_GLYPH_INVISIBLE;
            g->x_offset = g->y_offset = 0;
            g->advance = cp == '\t' ? hb_font_get_glyph_h_advance(font->font, space_glyph(font)) : 0;
        }
    }
    *glyphs = s->glyphs;
    *count = n;
    return UI_OK;
}

// ---- Outlines ----------------------------------------------------------------------------

bool ui_font_draw_glyph(ui_font *f, uint32_t glyph, const ui_path_sink *sink) {
    ui_path_sink copy = *sink; // HarfBuzz takes non-const draw data
    return hb_font_draw_glyph_or_fail(f->font, glyph, f->face->draw, &copy);
}

bool ui_font_glyph_bounds(const ui_font *f, uint32_t glyph, float *x0, float *y0, float *x1,
                          float *y1) {
    hb_glyph_extents_t e;
    if (!hb_font_get_glyph_extents(f->font, glyph, &e) || !e.width || !e.height) return false;
    // x_bearing/y_bearing is the top-left corner; height is negative (y up).
    *x0 = (float)e.x_bearing / 64.f;
    *x1 = (float)(e.x_bearing + e.width) / 64.f;
    *y1 = (float)e.y_bearing / 64.f;
    *y0 = (float)(e.y_bearing + e.height) / 64.f;
    return true;
}
