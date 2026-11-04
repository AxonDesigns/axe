#include "core/prepare.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "wire/gen/wire_gen.h"

#define MODULE "prepare"
#define GLYPHS_PER_JOB 64u

// Per-thread scratch: worker i uses workers[i]; jobs run inline use the last one.
typedef struct prep_worker {
    bool ready;
    ui_text_shaper shaper;
    ui_raster raster;
} prep_worker;

struct ui_preparer {
    const ui_logger *log;
    ui_hooks *hooks;
    ui_jobs *jobs;
    ui_font_registry *fonts;
    ui_glyph_cache *glyphs;
    void (*wake)(void *user);
    void *wake_user;
    ui_styles *strings;
    // Orders a text item's "done at generation g" against registrations, which bump the
    // generation and re-prepare items found done at an older one (ui_prep_head_step).
    mtx_t lock;
    prep_worker *workers;
    uint32_t worker_count;
    uint64_t next_seq;
    ui_job_group discovery;
    bool discovery_started;
    _Atomic uint64_t texts, shapes, glyphs_done;
    uint64_t reshapes; // core only
};

// A glyph to rasterize at every subpixel bin.
typedef struct glyph_ref {
    ui_font *font;
    ui_font_key key;
    uint32_t glyph;
} glyph_ref;

typedef struct raster_job {
    ui_preparer *p;
    uint64_t seq;
    uint32_t count;
    glyph_ref glyphs[];
} raster_job;

ui_result ui_preparer_create(const ui_preparer_desc *d, ui_preparer **out) {
    ui_preparer *p = calloc(1, sizeof *p);
    if (!p) return UI_ERR_OUT_OF_MEMORY;
    p->log = d->log;
    p->hooks = d->hooks;
    p->jobs = d->jobs;
    p->fonts = d->fonts;
    p->glyphs = d->glyphs;
    p->wake = d->wake;
    p->wake_user = d->wake_user;
    p->strings = d->strings;
    p->worker_count = ui_jobs_threads(d->jobs) + 1;
    p->workers = calloc(p->worker_count, sizeof *p->workers);
    if (!p->workers || mtx_init(&p->lock, mtx_plain) != thrd_success) {
        free(p->workers);
        free(p);
        return UI_ERR_OUT_OF_MEMORY;
    }
    atomic_init(&p->texts, 0u);
    atomic_init(&p->shapes, 0u);
    atomic_init(&p->glyphs_done, 0u);
    *out = p;
    return UI_OK;
}

void ui_preparer_destroy(ui_preparer *p) {
    if (!p) return;
    for (uint32_t i = 0; i < p->worker_count; i++) {
        if (!p->workers[i].ready) continue;
        ui_text_shaper_free(&p->workers[i].shaper);
        ui_raster_free(&p->workers[i].raster);
    }
    free(p->workers);
    mtx_destroy(&p->lock);
    free(p);
}

static prep_worker *worker(ui_preparer *p, const ui_job_ctx *ctx) {
    prep_worker *w = &p->workers[ctx->worker < p->worker_count ? ctx->worker : p->worker_count - 1];
    if (!w->ready && ui_text_shaper_init(&w->shaper) == UI_OK) w->ready = true;
    return w->ready ? w : NULL;
}

static void discovery_job(void *data, const ui_job_ctx *ctx) {
    if (!ctx->cancelled) ui_font_registry_start_discovery(data);
}

void ui_preparer_start_discovery(ui_preparer *p) {
    if (p->discovery_started || !ui_font_registry_discovery_pending(p->fonts)) return;
    p->discovery_started = true;
    ui_job_group_init(&p->discovery, p->wake, p->wake_user);
    if (ui_jobs_submit(p->jobs, &p->discovery, discovery_job, p->fonts) != UI_OK)
        ui_font_registry_start_discovery(p->fonts); // out of memory: do it here
    ui_job_group_seal(&p->discovery);
}

bool ui_preparer_discovery_ready(ui_preparer *p) {
    return !ui_font_registry_discovery_pending(p->fonts) &&
           (!p->discovery_started || ui_job_group_idle(&p->discovery));
}

void ui_preparer_get_stats(ui_preparer *p, ui_preparer_stats *out) {
    out->texts = atomic_load_explicit(&p->texts, memory_order_relaxed);
    out->shapes = atomic_load_explicit(&p->shapes, memory_order_relaxed);
    out->glyphs = atomic_load_explicit(&p->glyphs_done, memory_order_relaxed);
    out->reshapes = p->reshapes;
}

// ---- Glyphs ------------------------------------------------------------------------------

static void raster_job_run(void *data, const ui_job_ctx *ctx) {
    raster_job *j = data;
    prep_worker *w = ctx->cancelled ? NULL : worker(j->p, ctx);
    if (w) UI_HOOK(j->p->hooks, UI_TRACE_PREPARE_BEGIN, 0, (ui_handle){0}, j->seq);
    for (uint32_t i = 0; w && i < j->count; i++) {
        const glyph_ref *g = &j->glyphs[i];
        for (uint32_t bin = 0; bin < UI_ATLAS_SUBPIXEL_BINS; bin++) {
            ui_glyph_key key = {g->key, g->glyph, bin};
            if (ui_glyph_cache_protect(j->p->glyphs, key, j->seq)) continue;
            ui_bitmap bm;
            ui_result res = ui_raster_glyph(&w->raster, g->font, g->glyph,
                                            (float)bin / UI_ATLAS_SUBPIXEL_BINS, &bm);
            if (res == UI_ERR_OUT_OF_MEMORY) continue; // the core rasterizes it when drawn
            if (ui_glyph_cache_put(j->p->glyphs, key, res == UI_OK ? &bm : NULL, j->seq) == UI_OK)
                atomic_fetch_add_explicit(&j->p->glyphs_done, 1u, memory_order_relaxed);
        }
    }
    if (w) UI_HOOK(j->p->hooks, UI_TRACE_PREPARE_END, 0, (ui_handle){0}, j->seq);
    free(j);
}

static int glyph_order(const void *a, const void *b) {
    const glyph_ref *x = a, *y = b;
    if (x->key.face != y->key.face) return x->key.face < y->key.face ? -1 : 1;
    if (x->key.instance != y->key.instance) return x->key.instance < y->key.instance ? -1 : 1;
    if (x->key.size_q != y->key.size_q) return x->key.size_q < y->key.size_q ? -1 : 1;
    return x->glyph < y->glyph ? -1 : x->glyph > y->glyph;
}

// Queues rasterization of every distinct glyph of the shaped paragraphs, in chunks so a
// long paragraph spreads over the workers.
static void submit_glyphs(ui_prep_buffer *b, const ui_shaped_paragraph *shaped, uint32_t count) {
    ui_preparer *p = b->owner;
    uint32_t total = 0;
    for (uint32_t s = 0; s < count; s++) total += shaped[s].glyph_count;
    if (!total) return;
    glyph_ref *all = malloc(total * sizeof *all);
    if (!all) return; // the core rasterizes what it draws
    uint32_t n = 0;
    for (uint32_t s = 0; s < count; s++) {
        for (uint32_t r = 0; r < shaped[s].run_count; r++) {
            const ui_shaped_run *run = &shaped[s].runs[r];
            for (uint32_t i = 0; i < run->glyph_count; i++) {
                uint32_t glyph = shaped[s].glyphs[run->glyph_start + i].glyph;
                if (glyph != UI_GLYPH_INVISIBLE && run->sized_font)
                    all[n++] = (glyph_ref){run->sized_font, run->font, glyph};
            }
        }
    }
    qsort(all, n, sizeof *all, glyph_order);
    uint32_t unique = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!unique || glyph_order(&all[unique - 1], &all[i])) all[unique++] = all[i];
    }
    for (uint32_t at = 0; at < unique; at += GLYPHS_PER_JOB) {
        uint32_t k = unique - at < GLYPHS_PER_JOB ? unique - at : GLYPHS_PER_JOB;
        raster_job *j = malloc(sizeof *j + k * sizeof(glyph_ref));
        if (!j) break;
        j->p = p;
        j->seq = b->seq;
        j->count = k;
        memcpy(j->glyphs, all + at, k * sizeof(glyph_ref));
        if (ui_jobs_submit(p->jobs, &b->group, raster_job_run, j) != UI_OK) free(j);
    }
    free(all);
}

// ---- Text --------------------------------------------------------------------------------

// Shapes `tp` at each of its scales until the result is current: a registration that lands
// meanwhile makes it shape again, here or through ui_prep_head_step.
static void shape_item(ui_prep_item *it, prep_worker *w) {
    ui_prep_buffer *b = it->buf;
    ui_preparer *p = b->owner;
    ui_scene_text_prep *tp = &b->scene.texts[it->index];
    // What the node's values make of its set_text (core/scene.h, ui_text_ctx_key).
    ui_text_source *source = tp->op.source;
    bool ok = w != NULL;
    if (it->has_ctx) {
        ui_text_resolved key = ui_text_ctx_key(&it->ctx, source);
        ui_text_source *derived;
        if (!ui_text_source_shapes_as(source, &key)) {
            if (ok && ui_text_source_derive(tp->op.source, &key, &derived) == UI_OK) source = derived;
            else ok = false;
        }
    }
    if (source == tp->op.source) ui_text_source_retain(source); // a derived one comes with one
    ui_text_source_release(tp->shaped_source); // a previous preparation's
    tp->shaped_source = source;
    ui_text_input in = ui_text_source_input(source);
    for (;;) {
        for (uint32_t i = 0; ok && i < tp->scale_count; i++) {
            ok = ui_shape_paragraph(&w->shaper, p->fonts, &in, tp->scales[i], &tp->shaped[i]) == UI_OK;
            if (ok) atomic_fetch_add_explicit(&p->shapes, 1u, memory_order_relaxed);
        }
        mtx_lock(&p->lock);
        uint64_t gen = ok && tp->scale_count ? tp->shaped[0].generation : 0;
        bool current = !ok || !tp->scale_count || gen == ui_font_registry_generation(p->fonts);
        if (current) {
            // A failure leaves nothing to adopt: the core shapes the text when it lays it out.
            if (!ok) {
                for (uint32_t i = 0; i < tp->scale_count; i++) ui_shaped_paragraph_free(&tp->shaped[i]);
                tp->scale_count = 0;
                ui_text_source_release(tp->shaped_source);
                tp->shaped_source = NULL;
            }
            it->generation = gen;
            it->done = true;
        }
        mtx_unlock(&p->lock);
        if (current) break;
    }
    submit_glyphs(b, tp->shaped, tp->scale_count);
}

static void text_job(void *data, const ui_job_ctx *ctx) {
    ui_prep_item *it = data;
    if (ctx->cancelled) return;
    ui_prep_buffer *b = it->buf;
    ui_preparer *p = b->owner;
    UI_HOOK(p->hooks, UI_TRACE_PREPARE_BEGIN, 0, (ui_handle){0}, b->seq);
    ui_scene_text_prep *tp = &b->scene.texts[it->index];
    if (!tp->decoded) {
        ui_wire_reader r = it->after;
        ui_text_op_decode(&it->op, &r, &tp->op);
        tp->decoded = true;
        atomic_fetch_add_explicit(&p->texts, 1u, memory_order_relaxed);
    }
    if (tp->op.source) {
        shape_item(it, worker(p, ctx));
    } else {
        mtx_lock(&p->lock);
        it->done = true;
        mtx_unlock(&p->lock);
    }
    UI_HOOK(p->hooks, UI_TRACE_PREPARE_END, 0, (ui_handle){0}, b->seq);
}

static void font_job(void *data, const ui_job_ctx *ctx) {
    ui_prep_font *f = data;
    if (ctx->cancelled) return;
    ui_hooks *hooks = f->buf->owner->hooks;
    UI_HOOK(hooks, UI_TRACE_PREPARE_BEGIN, 0, (ui_handle){0}, f->buf->seq);
    f->result = ui_face_load_file(f->path, 0, &f->face);
    UI_HOOK(hooks, UI_TRACE_PREPARE_END, 0, (ui_handle){0}, f->buf->seq);
}

static void reshape_job(void *data, const ui_job_ctx *ctx) {
    ui_prep_reshape *r = data;
    if (ctx->cancelled) return;
    ui_preparer *p = r->buf->owner;
    UI_HOOK(p->hooks, UI_TRACE_PREPARE_BEGIN, 0, (ui_handle){0}, r->buf->seq);
    prep_worker *w = worker(p, ctx);
    ui_text_input in = ui_text_source_input(r->source);
    r->ok = w && ui_shape_paragraph(&w->shaper, p->fonts, &in, r->scale, &r->shaped) == UI_OK;
    if (r->ok) {
        atomic_fetch_add_explicit(&p->shapes, 1u, memory_order_relaxed);
        submit_glyphs(r->buf, &r->shaped, 1);
    }
    UI_HOOK(p->hooks, UI_TRACE_PREPARE_END, 0, (ui_handle){0}, r->buf->seq);
}

// ---- Buffers -----------------------------------------------------------------------------

ui_prep_buffer *ui_prep_create(ui_preparer *p, const uint8_t *data, size_t size) {
    ui_wire_reader r;
    if (ui_wire_reader_init(&r, UI_WIRE_MAGIC_CMD, data, size) != UI_OK) return NULL;
    uint32_t texts = 0, fonts = 0;
    bool styled = false; // ops that may change text values (the head step checks precisely)
    ui_wire_op op;
    while (ui_wire_reader_next(&r, &op) == UI_OK) {
        const char *path;
        uint32_t id, len;
        if (op.opcode == UI_OP_SET_TEXT) texts++;
        if (op.opcode == UI_OP_LOAD_FONT && ui_scene_font_path_valid(op.payload, &id, &path, &len))
            fonts++;
        styled |= op.opcode == UI_OP_DEFINE_STYLE || op.opcode == UI_OP_DESTROY_STYLE ||
                  op.opcode == UI_OP_SET_TOKEN || op.opcode == UI_OP_SET_STYLES ||
                  op.opcode == UI_OP_SET_PROPS || op.opcode == UI_OP_SET_STATE ||
                  op.opcode == UI_OP_MOVE_NODE || op.opcode == UI_OP_SET_TOKEN_OVERRIDES;
    }
    if (!texts && !fonts && !styled) return NULL;
    ui_prep_buffer *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->owner = p;
    b->styled = styled || texts;
    b->seq = ++p->next_seq;
    b->data = data;
    b->size = size;
    b->scene.texts = calloc(texts ? texts : 1, sizeof *b->scene.texts);
    b->items = calloc(texts ? texts : 1, sizeof *b->items);
    b->fonts = calloc(fonts ? fonts : 1, sizeof *b->fonts);
    if (!b->scene.texts || !b->items || !b->fonts) goto oom;
    ui_wire_reader_init(&r, UI_WIRE_MAGIC_CMD, data, size);
    while (ui_wire_reader_next(&r, &op) == UI_OK) {
        uint32_t offset = (uint32_t)(op.payload - UI_WIRE_OP_HEADER_SIZE - data);
        const char *path;
        uint32_t id, len;
        if (op.opcode == UI_OP_SET_TEXT) {
            uint32_t i = b->scene.text_count++;
            b->scene.texts[i].offset = offset;
            b->items[i] = (ui_prep_item){.buf = b, .index = i, .op = op, .after = r};
        } else if (op.opcode == UI_OP_LOAD_FONT &&
                   ui_scene_font_path_valid(op.payload, &id, &path, &len)) {
            ui_prep_font *f = &b->fonts[b->font_count];
            f->buf = b;
            f->offset = offset;
            f->path = malloc(len + 1);
            if (!f->path) goto oom;
            memcpy(f->path, path, len);
            f->path[len] = 0;
            f->result = UI_ERR_IO;
            b->font_count++;
        }
    }
    return b;
oom:
    ui_prep_free(b);
    return NULL;
}

void ui_prep_start(ui_prep_buffer *b, const float *scales, uint32_t scale_count) {
    ui_preparer *p = b->owner;
    b->started = true;
    ui_job_group_init(&b->group, p->wake, p->wake_user);
    for (uint32_t i = 0; i < b->scene.text_count; i++) {
        ui_scene_text_prep *tp = &b->scene.texts[i];
        tp->scale_count = scale_count < UI_MAX_SURFACES ? scale_count : UI_MAX_SURFACES;
        memcpy(tp->scales, scales, tp->scale_count * sizeof *scales);
        // Out of memory: the op is decoded and shaped on the core when applied.
        if (ui_jobs_submit(p->jobs, &b->group, text_job, &b->items[i]) != UI_OK)
            b->items[i].done = true;
    }
    for (uint32_t i = 0; i < b->font_count; i++) {
        if (ui_jobs_submit(p->jobs, &b->group, font_job, &b->fonts[i]) != UI_OK)
            font_job(&b->fonts[i], &(ui_job_ctx){p->worker_count - 1, false});
    }
    ui_job_group_seal(&b->group);
}

void ui_prep_guess(ui_prep_buffer *b, uint32_t index, const ui_text_ctx *ctx) {
    ui_prep_item *it = &b->items[index];
    ui_styles *strings = b->owner->strings;
    if (it->has_ctx) {
        ui_styles_release(strings, it->ctx.values.family);
        ui_styles_release(strings, it->ctx.values.language);
    }
    it->ctx = *ctx;
    it->has_ctx = true;
    // Kept in the scene's table (a preview's strings go with the preview).
    const char *f = ctx->values.family, *l = ctx->values.language;
    it->ctx.values.family = f ? ui_styles_intern(strings, f, ui_istr_len(f)) : NULL;
    it->ctx.values.language = l ? ui_styles_intern(strings, l, ui_istr_len(l)) : NULL;
    if ((f && !it->ctx.values.family) || (l && !it->ctx.values.language)) {
        ui_styles_release(strings, it->ctx.values.family);
        ui_styles_release(strings, it->ctx.values.language);
        it->has_ctx = false; // out of memory: shaped with set_text's values, then on the core
    }
}

bool ui_prep_register_fonts(ui_prep_buffer *b, ui_prep_buffer *const *pending, uint32_t pending_count) {
    ui_preparer *p = b->owner;
    b->registered = true;
    if (!b->font_count) return false;
    // Registering and finding text shaped before it happen under the lock that text jobs
    // take to declare themselves done, so every item is either found here or sees the new
    // generation itself (shape_item).
    mtx_lock(&p->lock);
    for (uint32_t i = 0; i < b->font_count; i++) {
        ui_prep_font *f = &b->fonts[i];
        if (!f->face) {
            UI_WARN(p->log, MODULE, "cannot load %s: %s", f->path, ui_result_str(f->result));
            continue;
        }
        bool changed;
        f->result = ui_font_registry_register(p->fonts, f->path, 0, f->face, NULL, &changed);
        f->face = NULL; // the registry owns it now (or destroyed it)
        if (changed) b->fonts_changed = true;
    }
    b->generation = ui_font_registry_generation(p->fonts);
    for (uint32_t k = 0; b->fonts_changed && k < pending_count; k++) {
        ui_prep_buffer *pb = pending[k];
        for (uint32_t i = 0; pb->started && i < pb->scene.text_count; i++) {
            ui_prep_item *it = &pb->items[i];
            it->requeue = it->done && pb->scene.texts[i].op.source &&
                          pb->scene.texts[i].scale_count && it->generation < b->generation;
            if (it->requeue) it->done = false;
        }
    }
    mtx_unlock(&p->lock);
    for (uint32_t k = 0; b->fonts_changed && k < pending_count; k++) {
        ui_prep_buffer *pb = pending[k];
        bool open = false;
        for (uint32_t i = 0; pb->started && i < pb->scene.text_count; i++) {
            ui_prep_item *it = &pb->items[i];
            if (!it->requeue) continue;
            it->requeue = false;
            if (!open) ui_job_group_reopen(&pb->group);
            open = true;
            if (ui_jobs_submit(p->jobs, &pb->group, text_job, it) == UI_OK) continue;
            // Out of memory: drop the stale shaping; the core shapes it when laid out.
            ui_scene_text_prep *tp = &pb->scene.texts[i];
            for (uint32_t s = 0; s < tp->scale_count; s++) ui_shaped_paragraph_free(&tp->shaped[s]);
            tp->scale_count = 0;
            it->done = true;
        }
        if (open) ui_job_group_seal(&pb->group);
    }
    return b->fonts_changed;
}

void ui_prep_restyle_item(ui_prep_buffer *b, uint32_t index, const ui_text_ctx *ctx) {
    ui_prep_item *it = &b->items[index];
    b->owner->reshapes++;
    ui_prep_guess(b, index, ctx);
    mtx_lock(&b->owner->lock);
    it->done = false;
    mtx_unlock(&b->owner->lock);
    ui_job_group_reopen(&b->group);
    if (ui_jobs_submit(b->owner->jobs, &b->group, text_job, it) != UI_OK) {
        // Out of memory: what it was shaped with is dropped; the core shapes it when laid out.
        ui_scene_text_prep *tp = &b->scene.texts[index];
        for (uint32_t s = 0; s < tp->scale_count; s++) ui_shaped_paragraph_free(&tp->shaped[s]);
        tp->scale_count = 0;
        it->done = true;
    }
    ui_job_group_seal(&b->group);
}

void ui_prep_reshape_paragraphs(ui_prep_buffer *b, const ui_prep_paragraph *paras, uint32_t count) {
    if (!count) return;
    ui_prep_reshape *more = realloc(b->reshapes, (b->reshape_count + count) * sizeof *more);
    if (!more) return; // the core shapes them when laid out
    b->reshapes = more;
    b->owner->reshapes += count;
    ui_job_group_reopen(&b->group);
    for (uint32_t i = 0; i < count; i++) {
        ui_prep_reshape *r = &b->reshapes[b->reshape_count++];
        *r = (ui_prep_reshape){.buf = b, .node = paras[i].node, .scale = paras[i].scale,
                               .base = ui_text_source_retain(paras[i].base),
                               .source = ui_text_source_retain(paras[i].source)};
        if (ui_jobs_submit(b->owner->jobs, &b->group, reshape_job, r) != UI_OK) r->ok = false;
    }
    ui_job_group_seal(&b->group);
}

void ui_prep_head_done(ui_prep_buffer *b) { b->head_done = true; }

bool ui_prep_needs_head_step(const ui_prep_buffer *b) {
    return (b->font_count || b->styled) && !b->head_done;
}

bool ui_prep_idle(const ui_prep_buffer *b) { return b->started && ui_job_group_idle(&b->group); }

bool ui_prep_ready(const ui_prep_buffer *b) { return ui_prep_idle(b) && !ui_prep_needs_head_step(b); }

ui_prep_font *ui_prep_font_at(ui_prep_buffer *b, uint32_t offset) {
    for (uint32_t i = 0; i < b->font_count; i++) {
        if (b->fonts[i].offset == offset) return &b->fonts[i];
    }
    return NULL;
}

void ui_prep_free(ui_prep_buffer *b) {
    if (!b) return;
    for (uint32_t i = 0; i < b->scene.text_count; i++) {
        ui_scene_text_prep *tp = &b->scene.texts[i];
        ui_text_op_free(&tp->op);
        ui_text_source_release(tp->shaped_source);
        for (uint32_t s = 0; s < UI_MAX_SURFACES; s++) ui_shaped_paragraph_free(&tp->shaped[s]);
        if (b->items[i].has_ctx) {
            ui_styles_release(b->owner->strings, b->items[i].ctx.values.family);
            ui_styles_release(b->owner->strings, b->items[i].ctx.values.language);
        }
    }
    for (uint32_t i = 0; i < b->font_count; i++) {
        if (b->fonts[i].face) ui_face_destroy(b->fonts[i].face);
        free(b->fonts[i].path);
    }
    for (uint32_t i = 0; i < b->reshape_count; i++) {
        ui_text_source_release(b->reshapes[i].base);
        ui_text_source_release(b->reshapes[i].source);
        ui_shaped_paragraph_free(&b->reshapes[i].shaped);
    }
    free(b->scene.texts);
    free(b->items);
    free(b->fonts);
    free(b->reshapes);
    free(b);
}
