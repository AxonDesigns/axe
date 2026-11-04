// Preparing command buffers on the worker threads (docs/text.md, "Preparation").
//
// When the core receives a buffer it starts its preparation at once: every set_text is
// decoded and shaped at each surface scale, and its glyphs are rasterized into the glyph
// cache; every load_font file is read and parsed. The core applies a buffer only once its
// preparation is complete, at the first frame boundary after that, so the expensive part
// of new text never runs on the core thread and never stalls a frame.
//
// Text is shaped with its node's values (styles and inheritance), which the buffers before
// it may still change, and fonts change every paragraph's shaping. So a buffer that sets
// text, changes text values or loads fonts has a second step that depends on the scene:
// when it reaches the head of the queue (every earlier buffer is applied, so the scene can't
// change until it applies), the core runs it (core.c, head_step):
//  - its own text is prepared again where the values it was shaped with were wrong
//    (ui_prep_restyle_item: they were guessed from the scene when it was received). This
//    comes first: it reads what each item was shaped as, which is only safe while none of
//    the buffer's jobs runs;
//  - its faces are registered (ui_prep_register_fonts); text prepared before is prepared
//    again in it and in later buffers (not text already being prepared again: it sees the
//    new fonts itself);
//  - existing paragraphs whose values it changes, or every paragraph after a font load, are
//    shaped again in its group (ui_prep_reshape_paragraphs).
// It applies once all of that is done, so the frame that applies it shows every paragraph
// in its new shaping.
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
    // The node's text context it is shaped with (strings interned in the preparer's table,
    // with references); has_ctx false: none known, set_text's own values.
    ui_text_ctx ctx;
    bool has_ctx;
} ui_prep_item;

typedef struct ui_prep_font {
    struct ui_prep_buffer *buf;
    uint32_t offset;       // byte offset of the load_font op
    char *path;            // NUL-terminated copy
    ui_face *face;         // loaded by a worker; owned by the registry once registered
    ui_result result;      // what font_loaded reports
} ui_prep_font;

// An existing paragraph shaped again, after a font load or with new values.
typedef struct ui_prep_reshape {
    struct ui_prep_buffer *buf;
    ui_handle node;
    ui_text_source *base;   // its set_text (a reference)
    ui_text_source *source; // what is shaped: base, or derived from it (a reference)
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
    bool styled;           // it sets text or may change text values: it needs the head step
    bool head_done;        // the head step ran
    bool registered;       // its fonts were registered
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
    ui_styles *strings;       // the scene's styles: text contexts' strings are interned there
} ui_preparer_desc;

typedef struct ui_preparer_stats {
    uint64_t texts;        // set_text ops prepared
    uint64_t shapes;       // paragraphs shaped on workers (reshapes after font loads included)
    uint64_t reshapes;     // set_text prepared again, or existing paragraphs shaped again
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
// Sets the text context item `index` is shaped with when started: the core's guess from the
// scene when the buffer arrives (core/preview.h).
void ui_prep_guess(ui_prep_buffer *b, uint32_t index, const ui_text_ctx *ctx);
// Submits the buffer's jobs, shaping text at `scales`. Discovery must be ready.
void ui_prep_start(ui_prep_buffer *b, const float *scales, uint32_t scale_count);
// The buffer still needs the head step (see the top).
bool ui_prep_needs_head_step(const ui_prep_buffer *b);
// The head step's parts, run by the core with the buffer at the head and idle.
// Registers the buffer's fonts; text in `pending` (the started buffers from the head on, b
// included) shaped before is prepared again. True if the registered faces changed.
bool ui_prep_register_fonts(ui_prep_buffer *b, ui_prep_buffer *const *pending, uint32_t pending_count);
// Prepares item `index` again with the context it will really have.
void ui_prep_restyle_item(ui_prep_buffer *b, uint32_t index, const ui_text_ctx *ctx);
// Shapes existing paragraphs again in the buffer's group: each `source` (base, or derived
// from base with the node's new values), offered when the buffer applies.
typedef struct ui_prep_paragraph {
    ui_handle node;
    ui_text_source *base, *source;
    float scale;
} ui_prep_paragraph;
void ui_prep_reshape_paragraphs(ui_prep_buffer *b, const ui_prep_paragraph *paras, uint32_t count);
void ui_prep_head_done(ui_prep_buffer *b);
// True once every submitted job has finished. Thread: any.
bool ui_prep_idle(const ui_prep_buffer *b);
// Ready to apply: started, idle, and the head step done if it needs one.
bool ui_prep_ready(const ui_prep_buffer *b);
// The prepared load_font at `offset`, or NULL.
ui_prep_font *ui_prep_font_at(ui_prep_buffer *b, uint32_t offset);
// Call only when idle (or after ui_jobs_destroy).
void ui_prep_free(ui_prep_buffer *b);
