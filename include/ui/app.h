// The app instance: owns the core, logic and render actors, the platform connection,
// surfaces, the animation clock and the GPU device. There is no global state; several apps
// may coexist in one process.
//
// Thread model: the thread that calls ui_app_create is the app's main thread and runs the
// core. Every ui_app_* function must be called from it unless stated otherwise.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui/handle.h"
#include "ui/result.h"
#include "ui/wire.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ui_app ui_app;

typedef enum ui_scheduler_kind {
    UI_SCHEDULER_FROM_ENV = 0, // UI_SCHEDULER=threaded|single, default threaded
    UI_SCHEDULER_THREADED,     // core on the main thread, logic and render on their own
    UI_SCHEDULER_SINGLE,       // core, logic, render run in order each frame on the main thread
} ui_scheduler_kind;

typedef enum ui_surface_kind {
    UI_SURFACE_WINDOW = 0, // a top-level window on the platform's windowing system
    UI_SURFACE_HEADLESS,   // renders to an offscreen texture; no window, no display needed
} ui_surface_kind;

typedef struct ui_surface_desc {
    ui_surface_kind kind;
    uint32_t width, height; // initial logical size; the compositor may override for windows
    uint32_t scale_120;     // headless only: scale in 120ths (0 means 120 = 1.0)
    const char *title;      // windows only; copied
} ui_surface_desc;

// The stand-in for a language runtime. Its callbacks run on the logic actor (the logic
// thread, or the main thread under the single scheduler), never concurrently, and talk to
// the core only through the buffers: they read event buffers and append commands to
// `cmds`, which the logic actor flushes to the core after each callback.
typedef struct ui_logic_desc {
    void *user;
    // Called once before any event. Optional.
    ui_result (*start)(void *user, ui_cmd_writer *cmds);
    // Called with each event buffer, in order. `events` is valid only during the call.
    ui_result (*on_events)(void *user, const void *events, size_t size, ui_cmd_writer *cmds);
    // Called once when the app shuts down. Optional.
    void (*stop)(void *user);
} ui_logic_desc;

// Host time source for the animation clock; defaults to CLOCK_MONOTONIC.
typedef struct ui_time_source {
    uint64_t (*now_ns)(void *user); // must be monotonic; called on the core and platform
    void *user;
} ui_time_source;

typedef enum ui_gpu_preference {
    UI_GPU_FROM_ENV = 0, // UI_GPU=software|hardware, default hardware
    UI_GPU_HARDWARE,
    UI_GPU_SOFTWARE,     // the fallback (CPU) adapter, e.g. lavapipe
} ui_gpu_preference;

// Fonts and text rendering.
typedef struct ui_text_config {
    const char *const *font_files; // fonts to register (TTF, OTF; face 0 of each)
    uint32_t font_file_count;
    bool system_fonts;             // find families and fallbacks among installed fonts
    // Coverage contrast boost for text (0 = linear coverage), and an optional coverage
    // gamma (0 or 1 = off). See docs/text.md. 0 picks the defaults; UI_TEXT_CONTRAST and
    // UI_TEXT_GAMMA override them.
    float contrast;
    float gamma;
    uint32_t atlas_page_size;      // px per side; 0 = 1024
    uint32_t atlas_max_pages;      // 0 = 8
    uint32_t glyph_evict_frames;   // 0 = 300
    // Bytes of glyph bitmaps that worker threads rasterize ahead of drawing; 0 = 16 MiB.
    uint32_t glyph_cache_bytes;
} ui_text_config;

typedef struct ui_app_desc {
    ui_scheduler_kind scheduler;
    ui_gpu_preference gpu;
    ui_surface_desc surface; // the app's first surface; more surfaces come later
    ui_logic_desc logic;
    ui_time_source time;     // optional; zeroed means CLOCK_MONOTONIC
    bool start_paused;       // start with the animation clock paused (tests)
    ui_text_config text;
    // Worker threads that prepare command buffers (shaping, glyphs, font loading) off the
    // core thread. 0: hardware threads minus 3, between 1 and 4; UI_WORKERS overrides.
    // The single scheduler always runs that work inline on the main thread.
    uint32_t worker_threads;
} ui_app_desc;

// Creates the app and its first surface, and starts the actors. Thread: becomes the main
// thread. Ownership: caller owns *out; destroy with ui_app_destroy.
ui_result ui_app_create(const ui_app_desc *desc, ui_app **out);

// Stops the actors (calling logic.stop), releases the GPU and the platform. Thread: main.
void ui_app_destroy(ui_app *app);

// Runs frames paced by the display until a surface asks to close or ui_app_quit is called.
// Thread: main. Returns UI_ERR_UNSUPPORTED for apps whose surfaces are all headless; drive
// those with ui_app_frame.
ui_result ui_app_run(ui_app *app);

// Makes ui_app_run return after the current frame. Thread: main.
void ui_app_quit(ui_app *app);

// Produces exactly one frame on every surface at the current animation-clock time and waits
// until the actors are idle: the frame is presented and the logic actor has handled the
// resulting events and flushed its commands (which apply on the next frame). With a paused
// clock this is deterministic and identical under both schedulers. Thread: main.
// Returns UI_ERR_WRONG_STATE while ui_app_run is active.
ui_result ui_app_frame(ui_app *app);

// ---- Animation clock -----------------------------------------------------------------
// Thread: main. Effects apply from the next frame.

void ui_app_clock_pause(ui_app *app, bool paused);
// Advances the clock by `delta_ns` (scaled time is not applied to steps).
void ui_app_clock_step(ui_app *app, uint64_t delta_ns);
// Sets the speed of the running clock; 1.0 is real time. Values <= 0 are rejected.
ui_result ui_app_clock_set_scale(ui_app *app, double scale);
uint64_t ui_app_clock_now(const ui_app *app);

// ---- Surfaces --------------------------------------------------------------------------

// Handle of the surface created from ui_app_desc.surface. Thread: main.
ui_handle ui_app_main_surface(const ui_app *app);

// Window positioning is not available on every platform; Wayland returns
// UI_ERR_UNSUPPORTED. Thread: main.
ui_result ui_surface_set_position(ui_app *app, ui_handle surface, int32_t x, int32_t y);

typedef struct ui_image {
    uint32_t width, height; // physical pixels
    uint32_t stride;        // bytes per row
    uint8_t *pixels;        // RGBA8, sRGB-encoded, premultiplied by an opaque background
} ui_image;

// Reads back the last frame rendered on a headless surface. Thread: main.
// Ownership: caller frees out->pixels with ui_image_free.
// Returns UI_ERR_UNSUPPORTED for window surfaces.
ui_result ui_surface_capture(ui_app *app, ui_handle surface, ui_image *out);
void ui_image_free(ui_image *image);
// Writes a binary PPM (P6); alpha is dropped. Thread: any.
ui_result ui_image_write_ppm(const ui_image *image, const char *path);

// Counters for tests and diagnostics. Thread: main.
typedef struct ui_app_stats {
    uint64_t frames_committed;
    uint64_t frames_skipped_idle;    // frame opportunities with nothing to draw
    uint64_t display_lists_replaced; // render fell behind; pending list replaced, not queued
    uint64_t commands_applied;
    uint64_t commands_dropped;       // stale or wrong-kind handles, unknown ops
    uint64_t buffers_rejected;       // malformed command buffers
    uint64_t text_invalid_sequences; // ill-formed UTF-8 subparts replaced with U+FFFD
    uint64_t glyphs_rasterized;      // glyphs added to the atlas (prepared or rasterized now)
    uint64_t glyphs_evicted;
    uint64_t glyphs_rejected;        // too large for the atlas, or no space left
    uint64_t atlas_updates;          // updates sent to the render thread
    uint32_t atlas_pages;
    uint32_t glyphs_cached;
    // Preparation off the core thread (command buffers are applied once prepared).
    uint64_t texts_prepared;            // set_text ops decoded and shaped on workers
    uint64_t glyphs_prepared;           // glyph bitmaps rasterized on workers
    uint64_t text_shaped_on_core;       // paragraphs the core had to shape itself
    uint64_t text_reshaped;             // paragraphs shaped again on workers (fonts, values)
    uint64_t glyphs_rasterized_on_core; // glyphs the core had to rasterize itself
    uint64_t buffers_deferred;          // buffers that missed a frame waiting for preparation
    uint64_t max_head_wait_ns;          // longest such wait at the head of the queue
} ui_app_stats;
void ui_app_get_stats(const ui_app *app, ui_app_stats *out);

#ifdef __cplusplus
}
#endif
