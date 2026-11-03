// Preparing command buffers on the worker threads (docs/text.md, "Preparation").
//
// When the core receives a buffer it starts its preparation at once: every set_text is
// decoded and shaped at each surface scale, and its glyphs are rasterized into the glyph
// cache; every load_font file is read and parsed. The core applies a buffer only once its
// preparation is complete, at the first frame boundary after that, so the expensive part
// of new text never runs on the core thread and never stalls a frame.
//
// Fonts change every paragraph's shaping, so a buffer with load_font has a second step that
// depends on the scene: when it reaches the head of the queue (every earlier buffer is
// applied, so the scene can't change until it applies), its faces are registered and every
// existing paragraph is shaped again in its group. Text already prepared in it and in later
// buffers that was shaped before the registration is prepared again.
//
// Thread: the core, except the jobs, which run on workers.
#pragma once

#include <threads.h>

#include "core/scene.h"
#include "hooks/hooks_internal.h"
#include "runtime/jobs.h"
#include "text/glyph_cache.h"

typedef struct ui_preparer ui_preparer;

// One set_text's preparation state, parallel to ui_prep_buffer.scene.texts.
typedef struct ui_prep_item {
    struct ui_prep_buffer *buf;
    uint32_t index;
    ui_wire_op op;         // the set_text
    ui_wire_reader after;  // positioned after it, where its continuations start
    bool done;             // shaped at `generation`; guarded by the preparer's lock
    bool requeue;          // shaped before a registration: prepare it again
    uint64_t generation;
} ui_prep_item;

typedef struct ui_prep_font {
    struct ui_prep_buffer *buf;
    uint32_t offset;       // byte offset of the load_font op
    char *path;            // NUL-terminated copy
    ui_face *face;         // loaded by a worker; owned by the registry once registered
    ui_result result;      // what font_loaded reports
} ui_prep_font;

// An existing paragraph shaped again after a font load.
typedef struct ui_prep_reshape {
    struct ui_prep_buffer *buf;
    ui_handle node;
    ui_text_source *source; // the source it was shaped from (a reference)
    float scale;
    ui_shaped_paragraph shaped;
    bool ok;
} ui_prep_reshape;

typedef struct ui_prep_buffer {
    ui_preparer *owner;
    uint64_t seq;          // increases per prepared buffer; protects its glyphs in the cache
    const uint8_t *data;   // the buffer (owned by the core's pending entry)
    size_t size;
    ui_job_group group;
    ui_scene_prepared scene;
    ui_prep_item *items;
    ui_prep_font *fonts;
    uint32_t font_count;
    ui_prep_reshape *reshapes;
    uint32_t reshape_count;
    bool started;          // jobs submitted
    bool registered;       // its fonts were registered (the head step ran)
    bool fonts_changed;    // registering changed the registered faces
    uint64_t generation;   // the registry generation after registering
} ui_prep_buffer;

typedef struct ui_preparer_desc {
    const ui_logger *log;
    ui_hooks *hooks;
    ui_jobs *jobs;
    ui_font_registry *fonts;
    ui_glyph_cache *glyphs;
    void (*wake)(void *user); // called (on any thread) whenever a buffer's jobs finish
    void *wake_user;
} ui_preparer_desc;

typedef struct ui_preparer_stats {
    uint64_t texts;        // set_text ops prepared
    uint64_t shapes;       // paragraphs shaped on workers (reshapes after font loads included)
    uint64_t glyphs;       // glyph bitmaps rasterized on workers
} ui_preparer_stats;

ui_result ui_preparer_create(const ui_preparer_desc *desc, ui_preparer **out);
// Call after ui_jobs_destroy, so no job is running.
void ui_preparer_destroy(ui_preparer *p);
// Creates the font discovery backend on a worker (the registry was made with
// defer_discovery). Buffers are started only once it is done.
void ui_preparer_start_discovery(ui_preparer *p);
bool ui_preparer_discovery_ready(ui_preparer *p);
void ui_preparer_get_stats(ui_preparer *p, ui_preparer_stats *out);

// Scans a received buffer whose framing is valid. NULL when it has nothing to prepare (or
// memory ran out, in which case the buffer is applied unprepared).
ui_prep_buffer *ui_prep_create(ui_preparer *p, const uint8_t *data, size_t size);
// Submits the buffer's jobs, shaping text at `scales`. Discovery must be ready.
void ui_prep_start(ui_prep_buffer *b, const float *scales, uint32_t scale_count);
// The buffer has fonts that still need the head step.
bool ui_prep_needs_head_step(const ui_prep_buffer *b);
// The head step (see the top): registers the buffer's fonts and, if that changed the
// registered faces, reshapes `paras` (every existing paragraph) and re-prepares stale text
// in `pending` (the started buffers from the head on, b included).
typedef struct ui_prep_paragraph {
    ui_handle node;
    ui_text_source *source;
    float scale;
} ui_prep_paragraph;
void ui_prep_head_step(ui_prep_buffer *b, ui_prep_buffer *const *pending, uint32_t pending_count,
                       const ui_prep_paragraph *paras, uint32_t para_count);
// True once every submitted job has finished. Thread: any.
bool ui_prep_idle(const ui_prep_buffer *b);
// Ready to apply: started, idle, and the head step done if it needs one.
bool ui_prep_ready(const ui_prep_buffer *b);
// The prepared load_font at `offset`, or NULL.
ui_prep_font *ui_prep_font_at(ui_prep_buffer *b, uint32_t offset);
// Call only when idle (or after ui_jobs_destroy).
void ui_prep_free(ui_prep_buffer *b);
