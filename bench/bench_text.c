// Text benchmarks. Meaningful in release builds:
//   cmake --build --preset release && build/release/bench/bench_text
// UI_GPU=software|hardware picks the adapter for the frame benchmark.
//  1. Shaping + layout of a 10,000-character multilingual paragraph (cold), and re-layout at
//     a new width (shaping cached).
//  2. Rasterizing 1,000 new glyphs into a fresh atlas.
//  3. Frame time with 2,000 glyphs on screen: a headless surface redrawn every frame (the
//     background changes, so the whole display list is rebuilt), split into core commit
//     (layout cache hits, glyph lookups, list building) and render encode.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/common.h"
#include "text/atlas.h"
#include "text/paragraph.h"
#include "ui/ui.h"

#define FONT(name) AXE_SOURCE_DIR "/tests/fonts/" name

static const char *const font_files[] = {
    FONT("NotoSans-VF.ttf"),       FONT("NotoSansHebrew-VF.ttf"), FONT("NotoSansArabic-VF.ttf"),
    FONT("NotoSansDevanagari-VF.ttf"), FONT("NotoSansSC-Subset.ttf"),
};

static const char SAMPLE[] =
    "The quick brown fox jumps over the lazy dog. "
    "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D \xD7\xA2\xD7\x95\xD7\x9C\xD7\x9D (123). "
    "\xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9 2026 "
    "\xE0\xA4\xB9\xE0\xA4\xBF\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA6\xE0\xA5\x80 \xE0\xA4\xAA\xE0\xA4\xBE\xE0\xA4\xA0 "
    "\xE6\x96\x87\xE5\xAD\x97\xE6\x8E\x92\xE7\x89\x88\xE3\x80\x82 ";

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static double median_us(uint64_t *ns, int n) {
    qsort(ns, (size_t)n, sizeof *ns, cmp_u64);
    return (double)ns[n / 2] / 1000.0;
}

static uint32_t count_cps(const char *s, size_t len) {
    uint32_t n = 0;
    for (size_t i = 0; i < len; i++) n += ((unsigned char)s[i] & 0xC0) != 0x80;
    return n;
}

static void bench_layout(ui_text_context *ctx) {
    // Repeat the sample to at least 10,000 code points.
    size_t cap = 64 * 1024, len = 0;
    char *text = malloc(cap);
    while (count_cps(text, len) < 10000) {
        memcpy(text + len, SAMPLE, sizeof SAMPLE - 1);
        len += sizeof SAMPLE - 1;
    }
    static const uint32_t offsets[] = {0, 9};
    ui_paragraph_desc d = {
        .text = (const uint8_t *)text, .len = (uint32_t)len,
        .base = {.family = 0, .weight = 400, .size = 16, .color = {0, 0, 0, 1}},
        .families = "Noto Sans", .family_offsets = offsets, .family_count = 1,
    };
    enum { RUNS = 15 };
    uint64_t cold[RUNS], relayout[RUNS];
    uint32_t lines = 0;
    for (int r = 0; r < RUNS; r++) {
        ui_paragraph p = {0};
        ui_paragraph_set(&p, &d);
        ui_result res;
        uint64_t t0 = ui_monotonic_ns();
        const ui_paragraph_layout *l = ui_paragraph_layout_get(&p, ctx, 1.25f, 800, &res);
        uint64_t t1 = ui_monotonic_ns();
        l = ui_paragraph_layout_get(&p, ctx, 1.25f, 640 + (float)r, &res);
        uint64_t t2 = ui_monotonic_ns();
        cold[r] = t1 - t0;
        relayout[r] = t2 - t1;
        lines = l ? l->line_count : 0;
        ui_paragraph_free(&p);
    }
    printf("shape+layout  %u code points (%zu bytes), %u lines: %.2f ms cold, %.2f ms re-layout\n",
           count_cps(text, len), len, lines, median_us(cold, RUNS) / 1000,
           median_us(relayout, RUNS) / 1000);
    free(text);
}

static void bench_raster(ui_font_registry *fonts) {
    enum { RUNS = 9, GLYPHS = 1000 };
    ui_font_key key;
    ui_font *font = ui_font_registry_font(fonts, 0, 20, 400, UI_FONT_NORMAL, &key);
    uint64_t t[RUNS];
    uint32_t drawn = 0;
    for (int r = 0; r < RUNS; r++) {
        ui_atlas *atlas;
        ui_atlas_create(&(ui_atlas_desc){0}, &atlas);
        ui_raster raster = {0};
        ui_atlas_begin_frame(atlas, 1, UINT64_MAX);
        uint64_t t0 = ui_monotonic_ns();
        drawn = 0;
        // Glyph IDs 1..1000 of Noto Sans: Latin, Greek, Cyrillic and Devanagari outlines.
        for (uint32_t g = 1; g <= GLYPHS; g++) {
            const ui_atlas_glyph *ag = ui_atlas_get(atlas, &raster, font, (ui_glyph_key){key, g, 0});
            drawn += ag && ag->width;
        }
        t[r] = ui_monotonic_ns() - t0;
        ui_raster_free(&raster);
        ui_atlas_destroy(atlas);
    }
    double us = median_us(t, RUNS);
    printf("rasterize     %u new glyphs at 20 px (%u with ink): %.2f ms, %.2f us/glyph\n", GLYPHS,
           drawn, us / 1000, us / (double)GLYPHS);
}

// ---- Frame benchmark ----------------------------------------------------------------------

typedef struct frame_logic {
    ui_handle_allocator *alloc;
    ui_handle bg, text;
    bool created;
    uint32_t frame;
    const char *content;
} frame_logic;

static ui_result on_events(void *user, const void *events, size_t size, ui_cmd_writer *cmds) {
    frame_logic *l = user;
    ui_event_reader rd;
    ui_result r = ui_event_reader_init(&rd, events, size);
    ui_event ev;
    while (r == UI_OK && ui_event_next(&rd, &ev) == UI_OK) {
        if (ev.type == UI_EVENT_SURFACE_RESIZED && !l->created) {
            l->created = true;
            ui_handle_alloc(l->alloc, &l->bg);
            ui_handle_alloc(l->alloc, &l->text);
            static const char *const fam[] = {"Noto Sans"};
            ui_text_desc t = {.text = l->content, .length = (uint32_t)strlen(l->content),
                              .style = {.family = 0, .weight = 400, .size = 14,
                                        .color = {0.1f, 0.1f, 0.1f, 1}},
                              .families = fam, .family_count = 1};
            ui_cmd_create_node(cmds, l->bg, ev.surface);
            ui_cmd_set_rect(cmds, l->bg, 0, 0, ev.surface_resized.width, ev.surface_resized.height);
            ui_cmd_set_color(cmds, l->bg, 1, 1, 1, 1);
            ui_cmd_create_node(cmds, l->text, l->bg);
            ui_cmd_set_rect(cmds, l->text, 16, 16, ev.surface_resized.width - 32,
                            ev.surface_resized.height - 32);
            ui_cmd_set_text(cmds, l->text, &t);
        } else if (ev.type == UI_EVENT_FRAME_COMMITTED && l->created) {
            // Change the background every frame so every frame rebuilds the whole list.
            float v = (l->frame++ & 1) ? 1.f : 0.98f;
            ui_cmd_set_color(cmds, l->bg, v, v, v, 1);
        }
    }
    return UI_OK;
}

typedef struct phase_times {
    _Atomic uint64_t start[UI_PHASE_COUNT];
    _Atomic uint64_t total[UI_PHASE_COUNT];
} phase_times;

static void on_trace(void *user, const ui_trace_event *ev) {
    phase_times *t = user;
    if (ev->kind == UI_TRACE_PHASE_BEGIN) atomic_store(&t->start[ev->phase], ev->time_ns);
    if (ev->kind == UI_TRACE_PHASE_END)
        atomic_fetch_add(&t->total[ev->phase], ev->time_ns - atomic_load(&t->start[ev->phase]));
}

static void bench_frames(void) {
    // 2,000 glyphs with ink: words of letters separated by spaces (spaces draw nothing).
    char content[4096];
    size_t len = 0;
    uint32_t ink = 0;
    for (uint32_t i = 0; ink < 2000; i++) {
        if (i % 7 == 6) {
            content[len++] = ' ';
        } else {
            content[len++] = (char)('a' + (i * 7 + i / 7) % 26);
            ink++;
        }
    }
    content[len] = 0;
    frame_logic logic = {.content = content};
    ui_handle_allocator_create(&logic.alloc);
    ui_app_desc desc = {
        .scheduler = UI_SCHEDULER_THREADED,
        .surface = {.kind = UI_SURFACE_HEADLESS, .width = 1280, .height = 720, .scale_120 = 150},
        .logic = {.user = &logic, .on_events = on_events},
        .start_paused = true,
        .text = {.font_files = font_files, .font_file_count = 1},
    };
    ui_app *app;
    if (ui_app_create(&desc, &app) != UI_OK) {
        printf("frames        skipped: no GPU adapter\n");
        return;
    }
    for (int i = 0; i < 20; i++) ui_app_frame(app); // warm up: glyphs cached, buffers sized
    phase_times pt = {0};
    uint32_t hook;
    bool hooks = ui_hooks_subscribe(app, on_trace, &pt, &hook) == UI_OK;
    enum { FRAMES = 200 };
    uint64_t t[FRAMES];
    for (int i = 0; i < FRAMES; i++) {
        uint64_t t0 = ui_monotonic_ns();
        ui_app_frame(app);
        t[i] = ui_monotonic_ns() - t0;
    }
    if (hooks) ui_hooks_unsubscribe(app, hook);
    ui_app_stats st;
    ui_app_get_stats(app, &st);
    printf("frames        %u glyphs at 1.25x, 1600x900 px: %.3f ms median per frame", ink,
           median_us(t, FRAMES) / 1000);
    if (hooks)
        printf(" (core commit %.3f ms, render encode %.3f ms)",
               (double)atomic_load(&pt.total[UI_PHASE_COMMIT]) / (double)FRAMES / 1e6,
               (double)atomic_load(&pt.total[UI_PHASE_ENCODE]) / (double)FRAMES / 1e6);
    printf("; %llu glyphs cached\n", (unsigned long long)st.glyphs_cached);
    ui_app_destroy(app);
    ui_handle_allocator_destroy(logic.alloc);
}

int main(void) {
    ui_logger log = {UI_LOG_WARN};
    ui_font_registry *fonts;
    ui_font_registry_create(&(ui_font_registry_desc){.log = &log}, &fonts);
    for (size_t i = 0; i < UI_ARRAY_LEN(font_files); i++)
        ui_font_registry_add_file(fonts, font_files[i], 0, NULL);
    ui_text_context ctx;
    ui_text_context_init(&ctx, fonts);
#ifndef NDEBUG
    printf("(debug build: numbers are not representative; use the release preset)\n");
#endif
    bench_layout(&ctx);
    bench_raster(fonts);
    bench_frames();
    ui_text_context_free(&ctx);
    ui_font_registry_destroy(fonts);
    return 0;
}
