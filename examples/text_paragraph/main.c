// text_paragraph: a multilingual paragraph on a card that follows the window and, every few
// seconds, springs between a narrow and a wide width, reflowing live.
//
//   text_paragraph [--period S]            window; the card springs every S seconds (3)
//   text_paragraph --headless [--out f.ppm] [--scale N]
//                                           offscreen at scale N/120, writes a PPM
//
// Fonts: the bundled test fonts plus installed system fonts (for fallback).
// Environment: UI_SCHEDULER=threaded|single, UI_GPU=software, UI_TRACE=1, UI_LOG=debug,
// UI_TEXT_CONTRAST=k.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/ui.h"

#define FONT(name) AXE_SOURCE_DIR "/tests/fonts/" name
#define TIMER_ID 1
#define MARGIN 24.f
#define PADDING 20.f
#define STIFFNESS 120.f
#define DAMPING 18.f

static const char *const font_files[] = {
    FONT("NotoSans-VF.ttf"),         FONT("NotoSansHebrew-VF.ttf"),
    FONT("NotoSansArabic-VF.ttf"),   FONT("NotoSansDevanagari-VF.ttf"),
    FONT("NotoSansSC-Subset.ttf"),
};

static const char PARAGRAPH[] =
    "Axe lays out text in many scripts at once. English sits next to Hebrew "
    "\xD7\xA2\xD7\x91\xD7\xA8\xD7\x99\xD7\xAA (\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D!) and Arabic "
    "\xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9 with numbers like 2026, Chinese "
    "\xE5\xA4\x9A\xE8\xAF\xAD\xE8\xA8\x80\xE6\x8E\x92\xE7\x89\x88\xEF\xBC\x9A\xE4\xB8\x80"
    "\xE4\xB8\xAA\xE6\xAE\xB5\xE8\x90\xBD\xEF\xBC\x8C\xE5\xAE\xBD\xE5\xBA\xA6\xE5\x8F\x98"
    "\xE5\x8C\x96\xE6\x97\xB6\xE9\x87\x8D\xE6\x96\xB0\xE6\x8D\xA2\xE8\xA1\x8C\xE3\x80\x82 and "
    "Devanagari \xE0\xA4\xB9\xE0\xA4\xBF\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA6\xE0\xA5\x80 "
    "\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xB7\xE0\xA4\xA4\xE0\xA5\x8D\xE0\xA4\xB0\xE0\xA4\xBF"
    "\xE0\xA4\xAF. Each line is shaped with HarfBuzz, reordered by the bidi algorithm and "
    "broken by the line-breaking rules, and the text reflows as the card's width springs.";

typedef struct demo {
    ui_handle_allocator *handles;
    ui_handle surface, card, text;
    float width, height;   // window, logical px
    bool wide;             // which width the card is heading for
    uint64_t period_ns;
    bool timer_armed;
} demo;

static float card_width(const demo *d) {
    float w = d->width - 2 * MARGIN;
    return (d->wide ? 1.f : 0.45f) * (w > 0 ? w : 0);
}

// Springs the card (and the text inside it) to the current target width.
static ui_result spring_to_target(demo *d, ui_cmd_writer *cmds) {
    float cw = card_width(d), ch = d->height - 2 * MARGIN;
    if (ch < 0) ch = 0;
    float card[4] = {MARGIN, MARGIN, cw, ch};
    float text[4] = {PADDING, PADDING, cw > 2 * PADDING ? cw - 2 * PADDING : 0,
                     ch > 2 * PADDING ? ch - 2 * PADDING : 0};
    ui_result r = ui_cmd_start_spring(cmds, d->card, UI_PROP_RECT, card, STIFFNESS, DAMPING);
    if (r == UI_OK) r = ui_cmd_start_spring(cmds, d->text, UI_PROP_RECT, text, STIFFNESS, DAMPING);
    return r;
}

static ui_result create(demo *d, ui_cmd_writer *cmds) {
    ui_result r = ui_handle_alloc(d->handles, &d->card);
    if (r == UI_OK) r = ui_handle_alloc(d->handles, &d->text);
    if (r != UI_OK) return r;
    static const char *const families[] = {"Noto Sans, sans-serif"};
    ui_text_desc text = {.set = UI_TEXT_SET_ALL,
        .text = PARAGRAPH,
        .length = sizeof PARAGRAPH - 1,
        .style = {.family = 0, .weight = 400, .size = 17, .color = {0.11f, 0.12f, 0.15f, 1}},
        .families = families,
        .family_count = 1,
    };
    float cw = card_width(d), ch = d->height - 2 * MARGIN;
    if ((r = ui_cmd_create_node(cmds, d->card, d->surface)) != UI_OK ||
        (r = ui_cmd_set_rect(cmds, d->card, MARGIN, MARGIN, cw, ch > 0 ? ch : 0)) != UI_OK ||
        (r = ui_cmd_set_color(cmds, d->card, 0.97f, 0.96f, 0.93f, 1)) != UI_OK ||
        (r = ui_cmd_set_corner_radius(cmds, d->card, 14)) != UI_OK ||
        (r = ui_cmd_create_node(cmds, d->text, d->card)) != UI_OK ||
        (r = ui_cmd_set_rect(cmds, d->text, PADDING, PADDING, cw - 2 * PADDING,
                             ch - 2 * PADDING)) != UI_OK ||
        (r = ui_cmd_set_text(cmds, d->text, &text)) != UI_OK)
        return r;
    return UI_OK;
}

static ui_result on_events(void *user, const void *events, size_t size, ui_cmd_writer *cmds) {
    demo *d = user;
    ui_event_reader rd;
    ui_result r = ui_event_reader_init(&rd, events, size);
    if (r != UI_OK) return r;
    ui_event ev;
    while ((r = ui_event_next(&rd, &ev)) == UI_OK) {
        switch (ev.type) {
        case UI_EVENT_SURFACE_RESIZED: {
            bool first = ui_handle_is_null(d->card);
            d->surface = ev.surface;
            d->width = ev.surface_resized.width;
            d->height = ev.surface_resized.height;
            r = first ? create(d, cmds) : spring_to_target(d, cmds);
            if (r != UI_OK) return r;
            break;
        }
        case UI_EVENT_FRAME_COMMITTED:
            if (!d->timer_armed && d->period_ns) {
                d->timer_armed = true;
                r = ui_cmd_set_timer(cmds, TIMER_ID,
                                     ev.frame_committed.predicted_present_ns + d->period_ns);
                if (r != UI_OK) return r;
            }
            break;
        case UI_EVENT_TIMER_FIRED:
            d->wide = !d->wide;
            if ((r = spring_to_target(d, cmds)) != UI_OK ||
                (r = ui_cmd_set_timer(cmds, TIMER_ID, ev.timer_fired.deadline_ns + d->period_ns)) != UI_OK)
                return r;
            break;
        default:
            break;
        }
    }
    return r == UI_ERR_END ? UI_OK : r;
}

int main(int argc, char **argv) {
    bool headless = false;
    double period = 3.0;
    uint32_t scale = 150;
    const char *out = "text_paragraph.ppm";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless")) headless = true;
        else if (!strcmp(argv[i], "--period") && i + 1 < argc) period = atof(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = (uint32_t)atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: %s [--period S] [--headless [--out f.ppm] [--scale N]]\n", argv[0]);
            return 2;
        }
    }
    demo d = {.wide = true, .period_ns = headless ? 0 : (uint64_t)(period * 1e9)};
    if (ui_handle_allocator_create(&d.handles) != UI_OK) return 1;
    ui_app_desc desc = {
        .surface = {.kind = headless ? UI_SURFACE_HEADLESS : UI_SURFACE_WINDOW,
                    .width = 900, .height = 520, .scale_120 = headless ? scale : 0,
                    .title = "Axe — text_paragraph"},
        .logic = {.user = &d, .on_events = on_events},
        .start_paused = headless,
        .text = {.font_files = font_files,
                 .font_file_count = sizeof font_files / sizeof *font_files,
                 .system_fonts = true},
    };
    ui_app *app;
    ui_result r = ui_app_create(&desc, &app);
    if (r != UI_OK) {
        fprintf(stderr, "cannot start: %s\n", ui_result_str(r));
        ui_handle_allocator_destroy(d.handles);
        return 1;
    }
    int status = 0;
    if (headless) {
        ui_image image;
        r = ui_app_frame(app);
        if (r == UI_OK) r = ui_app_frame(app);
        if (r == UI_OK) r = ui_surface_capture(app, ui_app_main_surface(app), &image);
        if (r == UI_OK) r = ui_image_write_ppm(&image, out);
        if (r == UI_OK) {
            printf("wrote %s (%ux%u)\n", out, image.width, image.height);
            ui_image_free(&image);
        } else {
            fprintf(stderr, "headless frame failed: %s\n", ui_result_str(r));
            status = 1;
        }
    } else {
        r = ui_app_run(app);
        if (r != UI_OK) {
            fprintf(stderr, "run failed: %s\n", ui_result_str(r));
            status = 1;
        }
    }
    ui_app_destroy(app);
    ui_handle_allocator_destroy(d.handles);
    return status;
}
