#include "core/core.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "core/preview.h"
#include "render/render.h"
#include "wire/gen/wire_gen.h"

#define MODULE "core"

// Background behind every surface's content: a dark neutral gray, linear.
static const float clear_color[4] = {0.012f, 0.013f, 0.016f, 1.0f};

// At most this many event buffers wait in logic's queue; later events coalesce into one
// buffer, so a stalled logic actor receives one batch instead of a backlog.
#define MAX_EVENTS_IN_FLIGHT 2u

static void core_wake(void *user) {
    ui_core *c = user;
    c->platform->vt->wake(c->platform);
    ui_signal_notify(c->progress);
}

// ---- Timers (animation clock) ----------------------------------------------------------

// Scene callback for set_timer ops, applied at the frame boundary like every command.
static void set_timer(void *user, uint32_t id, uint64_t deadline_ns) {
    ui_core *c = user;
    for (uint32_t i = 0; i < c->timer_count; i++) {
        if (c->timers[i].id != id) continue;
        if (deadline_ns == UI_WIRE_TIMER_CANCEL) c->timers[i] = c->timers[--c->timer_count];
        else c->timers[i].deadline_ns = deadline_ns;
        return;
    }
    if (deadline_ns == UI_WIRE_TIMER_CANCEL) return;
    if (c->timer_count == UI_MAX_TIMERS ||
        ui_grow((void **)&c->timers, &c->timer_capacity, c->timer_count + 1, sizeof *c->timers) !=
            UI_OK) {
        c->scene.stats.dropped++;
        UI_DEBUG_REPORT(c->log, MODULE, "dropped set_timer %u: too many timers", id);
        return;
    }
    c->timers[c->timer_count++] = (ui_core_timer){id, deadline_ns};
}

// Scene callback for load_font ops, applied at the frame boundary like every command. The
// file was read on a worker and registered when its buffer reached the head of the queue
// (core/prepare.h); here only the answer goes out. A buffer that could not be prepared
// loads the file now, on the core thread.
static void load_font(void *user, uint32_t id, const char *path, uint32_t len, uint32_t offset) {
    ui_core *c = user;
    ui_prep_font *f = c->applying ? ui_prep_font_at(c->applying, offset) : NULL;
    ui_result r;
    if (f) {
        r = f->result;
    } else {
        char buf[UI_FONT_PATH_MAX + 1];
        memcpy(buf, path, len);
        buf[len] = 0;
        uint64_t before = ui_font_registry_generation(c->fonts);
        r = ui_font_registry_add_file(c->fonts, buf, 0, NULL);
        if (ui_font_registry_generation(c->fonts) != before) {
            c->text.generation = ui_font_registry_generation(c->fonts);
            ui_scene_fonts_changed(&c->scene);
        }
    }
    if (ui_wire_font_loaded_put(&c->events, &(ui_wire_font_loaded){id, (uint32_t)r}) != UI_OK)
        UI_ERROR(c->log, MODULE, "out of memory queueing font events");
}

static void free_pending(ui_core *c, uint32_t i) {
    ui_prep_free(c->pending[i].prep);
    free(c->pending[i].data);
}

static int timer_order(const void *a, const void *b) {
    const ui_core_timer *x = a, *y = b;
    if (x->deadline_ns != y->deadline_ns) return x->deadline_ns < y->deadline_ns ? -1 : 1;
    return x->id < y->id ? -1 : x->id > y->id;
}

// Emits timer_fired for every due timer, earliest first (ties by id), and disarms them.
static void fire_timers(ui_core *c) {
    if (!c->timer_count) return;
    uint64_t now = ui_clock_at(&c->clock, ui_clock_host_now(&c->clock));
    qsort(c->timers, c->timer_count, sizeof *c->timers, timer_order);
    uint32_t due = 0;
    while (due < c->timer_count && c->timers[due].deadline_ns <= now) {
        ui_wire_timer_fired ev = {.id = c->timers[due].id, .deadline_ns = c->timers[due].deadline_ns, .now_ns = now};
        if (ui_wire_timer_fired_put(&c->events, &ev) != UI_OK) {
            UI_ERROR(c->log, MODULE, "out of memory queueing timer events");
            break;
        }
        due++;
    }
    memmove(c->timers, c->timers + due, (c->timer_count - due) * sizeof *c->timers);
    c->timer_count -= due;
}

static float env_float(const char *name, float fallback) {
    const char *v = getenv(name);
    if (!v || !*v) return fallback;
    char *end;
    float f = strtof(v, &end);
    return *end || !isfinite(f) || f < 0 ? fallback : f;
}

static ui_result init_text(ui_core *c, const ui_text_config *cfg) {
    static const ui_text_config none = {0};
    if (!cfg) cfg = &none;
    // Loading the system font configuration can take tens of milliseconds: it happens on a
    // worker, and buffers are prepared once it is done.
    ui_font_registry_desc fd = {.log = c->log, .system_fonts = cfg->system_fonts,
                                .defer_discovery = true};
    ui_result r = ui_font_registry_create(&fd, &c->fonts);
    if (r != UI_OK) return r;
    for (uint32_t i = 0; i < cfg->font_file_count; i++)
        ui_font_registry_add_file(c->fonts, cfg->font_files[i], 0, NULL); // failures are logged
    if (!ui_font_registry_face_count(c->fonts) && !cfg->system_fonts)
        UI_WARN(c->log, MODULE, "no fonts: text will not be drawn");
    r = ui_text_context_init(&c->text, c->fonts);
    if (r != UI_OK) return r;
    c->text.generation = ui_font_registry_generation(c->fonts);
    ui_atlas_desc ad = {cfg->atlas_page_size, cfg->atlas_max_pages, cfg->glyph_evict_frames};
    r = ui_atlas_create(&ad, &c->atlas);
    if (r != UI_OK) return r;
    r = ui_glyph_cache_create(cfg->glyph_cache_bytes, &c->glyphs);
    if (r != UI_OK) return r;
    ui_atlas_set_glyph_cache(c->atlas, c->glyphs);
    ui_preparer_desc pd = {
        .log = c->log, .hooks = c->hooks, .jobs = c->jobs, .fonts = c->fonts,
        .glyphs = c->glyphs, .wake = core_wake, .wake_user = c, .strings = &c->scene.styles,
    };
    r = ui_preparer_create(&pd, &c->preparer);
    if (r != UI_OK) return r;
    c->text_contrast = env_float("UI_TEXT_CONTRAST", cfg->contrast > 0 ? cfg->contrast : UI_TEXT_DEFAULT_CONTRAST);
    c->text_gamma = env_float("UI_TEXT_GAMMA", cfg->gamma > 0 ? cfg->gamma : 1.f);
    return UI_OK;
}

ui_result ui_core_init(ui_core *c, const ui_core_desc *desc) {
    memset(c, 0, sizeof *c);
    c->log = desc->log;
    c->hooks = desc->hooks;
    c->platform = desc->platform;
    c->progress = desc->progress;
    c->jobs = desc->jobs;
    ui_clock_init(&c->clock, desc->time, desc->start_paused);
    ui_scene_init(&c->scene, desc->log);
    ui_handle_table_init(&c->surface_handles, UI_HANDLE_SURFACE);
    ui_wire_buf_init(&c->events, UI_WIRE_MAGIC_EVENT);
    c->scene.timer_fn = set_timer;
    c->scene.timer_user = c;
    c->scene.font_fn = load_font;
    c->scene.font_user = c;
    ui_result r = ui_actor_init(&c->actor, "core", c, NULL, ui_core_step, NULL, 1024,
                                (ui_waker){core_wake, c});
    if (r != UI_OK) return r;
    r = ui_queue_init_unbounded(&c->from_logic, 256, (ui_waker){core_wake, c});
    if (r != UI_OK) {
        ui_actor_destroy(&c->actor);
        return r;
    }
    if (!c->jobs) {
        r = ui_jobs_create(&(ui_jobs_desc){.log = c->log, .threads = 0}, &c->own_jobs);
        c->jobs = c->own_jobs;
    }
    if (r == UI_OK) r = init_text(c, desc->text);
    if (r != UI_OK) {
        ui_jobs_destroy(c->own_jobs);
        ui_queue_destroy(&c->from_logic, NULL);
        ui_actor_destroy(&c->actor);
        ui_preparer_destroy(c->preparer); // discovery starts at the first step: no job runs
        ui_atlas_destroy(c->atlas);
        ui_glyph_cache_destroy(c->glyphs);
        ui_text_context_free(&c->text);
        ui_font_registry_destroy(c->fonts);
    }
    return r;
}

void ui_core_destroy(ui_core *c) {
    ui_queue_destroy(&c->from_logic, ui_msg_drop);
    ui_actor_destroy(&c->actor);
    free(c->timers);
    for (uint32_t i = 0; i < c->pending_count; i++) free_pending(c, i);
    free(c->pending);
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        for (int j = 0; j < 3; j++) ui_display_list_free(&c->surfaces[i].lists[j]);
    }
    ui_wire_buf_free(&c->events);
    ui_handle_table_destroy(&c->surface_handles);
    ui_scene_destroy(&c->scene); // before the fonts its paragraphs refer to
    free(c->unsent);
    ui_jobs_destroy(c->own_jobs); // inline: never holds queued work
    ui_preparer_destroy(c->preparer);
    ui_atlas_destroy(c->atlas);
    ui_glyph_cache_destroy(c->glyphs);
    ui_raster_free(&c->raster);
    ui_text_context_free(&c->text);
    ui_font_registry_destroy(c->fonts);
}

ui_core_surface *ui_core_surface_get(ui_core *c, ui_handle handle) {
    if (!ui_handle_table_valid(&c->surface_handles, handle)) return NULL;
    uint32_t slot = ui_handle_slot(handle);
    return slot < UI_MAX_SURFACES ? &c->surfaces[slot] : NULL;
}

// ---- Platform callbacks (main thread) ---------------------------------------------------

static void on_configure(void *user, void *tag, const ui_surface_config *config) {
    (void)user;
    ui_core_surface *s = tag;
    bool changed = !s->configured || s->config.logical_width != config->logical_width ||
                   s->config.logical_height != config->logical_height ||
                   s->config.scale_120 != config->scale_120;
    // Every new configure is acked by the next drawn frame (ui_wl_pre_present), so a
    // configure that changes only window state still needs one drawn frame.
    if (!s->configured || config->token != s->config.token) s->config_changed = true;
    s->config = *config;
    s->configured = true;
    if (changed) s->resized_pending = true;
}

static void on_frame(void *user, void *tag, uint64_t predicted_present_host_ns) {
    (void)user;
    ui_core_surface *s = tag;
    s->frame_ready = true;
    s->awaiting_frame = false;
    s->predicted_host_ns = predicted_present_host_ns;
}

static void on_close(void *user, void *tag) {
    ui_core *c = user;
    ui_core_surface *s = tag;
    if (ui_wire_surface_close_requested_put(&c->events, &(ui_wire_surface_close_requested){s->handle}) != UI_OK)
        UI_ERROR(c->log, MODULE, "out of memory queueing close event");
    // v0 policy: closing the window ends the app; logic still sees the event first.
    c->quit_requested = true;
}

static void on_presented(void *user, void *tag, uint64_t frame_id, uint64_t present_ns,
                         uint64_t refresh_ns) {
    ui_core *c = user;
    ui_core_surface *s = tag;
    (void)c, (void)s, (void)frame_id, (void)present_ns, (void)refresh_ns; // hooks may be off
    UI_HOOK_PRESENTED(c->hooks, s->handle, frame_id, present_ns, refresh_ns);
}

ui_platform_listener ui_core_platform_listener(ui_core *c) {
    return (ui_platform_listener){
        .user = c,
        .configure = on_configure,
        .frame = on_frame,
        .close = on_close,
        .presented = on_presented,
    };
}

// ---- Surfaces ----------------------------------------------------------------------------

ui_result ui_core_add_surface(ui_core *c, const ui_surface_desc *desc, ui_handle *out) {
    ui_handle h;
    ui_result r = ui_handle_table_alloc(&c->surface_handles, &h);
    if (r != UI_OK) return r;
    uint32_t slot = ui_handle_slot(h);
    if (slot >= UI_MAX_SURFACES) {
        ui_handle_table_release(&c->surface_handles, h);
        return UI_ERR_FULL;
    }
    ui_core_surface *s = &c->surfaces[slot];
    memset(s, 0, sizeof *s);
    s->core = c;
    s->handle = h;
    s->kind = desc->kind;
    ui_mailbox_init(&s->mailbox, &s->lists[0], &s->lists[1], &s->lists[2]);

    r = ui_scene_add_root(&c->scene, h);
    if (r == UI_OK) ui_scene_set_root_direction(&c->scene, h, desc->direction == UI_DIRECTION_RTL);
    if (r == UI_OK) r = c->platform->vt->surface_create(c->platform, desc, s, &s->platform);
    if (r != UI_OK) goto fail;
    if (!s->configured) {
        r = UI_ERR_PLATFORM;
        goto fail;
    }

    ui_render_surface_desc *rd = malloc(sizeof *rd);
    if (!rd) {
        r = UI_ERR_OUT_OF_MEMORY;
        goto fail;
    }
    *rd = (ui_render_surface_desc){
        .surface = h, .mailbox = &s->mailbox, .platform = c->platform, .psurface = s->platform,
    };
    c->platform->vt->surface_native(s->platform, &rd->native);
    ui_msg msg = {.type = UI_MSG_SURFACE_ADD, .surface = h, .ptr = rd};
    r = ui_queue_push(&c->render->queue, &msg);
    if (r != UI_OK) {
        free(rd);
        goto fail;
    }
    *out = h;
    return UI_OK;

fail:
    if (s->platform) c->platform->vt->surface_destroy(c->platform, s->platform);
    ui_scene_remove_root(&c->scene, h);
    ui_handle_table_release(&c->surface_handles, h);
    memset(s, 0, sizeof *s);
    return r;
}

void ui_core_remove_surfaces(ui_core *c) {
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        ui_core_surface *s = &c->surfaces[i];
        if (ui_handle_is_null(s->handle)) continue;
        ui_scene_remove_root(&c->scene, s->handle);
        if (s->platform) c->platform->vt->surface_destroy(c->platform, s->platform);
        s->platform = NULL;
        ui_handle_table_release(&c->surface_handles, s->handle);
        s->handle = (ui_handle){0};
    }
}

void ui_core_force_frame(ui_core *c) {
    uint64_t now = ui_clock_host_now(&c->clock);
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        ui_core_surface *s = &c->surfaces[i];
        if (ui_handle_is_null(s->handle) || !s->configured) continue;
        s->frame_ready = true;
        s->predicted_host_ns = now;
    }
}

// ---- Frame -------------------------------------------------------------------------------

static void handle(ui_core *c, ui_msg msg) {
    switch (msg.type) {
    case UI_MSG_CMD_BUFFER: {
        // drain() reserved the slot, so a buffer is never refused here. A well-framed buffer
        // starts preparing now (once font discovery is ready); a malformed one is rejected
        // when applied.
        ui_prep_buffer *prep = NULL;
        if (ui_scene_validate(msg.ptr, msg.size) == UI_OK)
            prep = ui_prep_create(c->preparer, msg.ptr, msg.size);
        c->pending[c->pending_count++] = (struct ui_pending_cmds){msg.ptr, msg.size, prep, false};
        break;
    }
    case UI_MSG_EVENTS_DONE:
        c->events_in_flight -= (uint32_t)msg.u64;
        break;
    case UI_MSG_FRAME_DONE: {
        ui_core_surface *s = ui_core_surface_get(c, msg.surface);
        if (s && msg.u64 > s->last_done_frame) s->last_done_frame = msg.u64;
        // A skipped frame was never committed, so no frame callback is coming; redraw at the
        // surface's next refresh instead of stalling.
        if (s && msg.size) {
            s->frame_ready = true;
            s->awaiting_frame = false;
            s->force_draw = true;
            s->predicted_host_ns = ui_clock_host_now(&c->clock) +
                                   c->platform->vt->surface_period_ns(s->platform);
        }
        break;
    }
    case UI_MSG_RENDER_FAILED:
        UI_ERROR(c->log, MODULE, "render failed: %s", ui_result_str((ui_result)msg.u64));
        c->failure = (ui_result)msg.u64;
        c->quit_requested = true;
        break;
    default:
        ui_msg_drop(&msg);
        break;
    }
}

static void drain(ui_core *c) {
    // Font discovery starts with the core's first step, not at init: its completion wakes
    // the platform, which an app creates after the core (and may fail to create).
    ui_preparer_start_discovery(c->preparer);
    ui_msg msg;
    // Reserve room for every queued command buffer before taking any, so the logic channel
    // stays lossless and ordered even when memory is short: on failure the messages simply
    // stay queued until the next step.
    uint32_t queued = ui_queue_count(&c->from_logic);
    if (queued && ui_grow((void **)&c->pending, &c->pending_capacity, c->pending_count + queued,
                          sizeof *c->pending) != UI_OK) {
        UI_ERROR(c->log, MODULE, "out of memory; %u logic message(s) wait for the next step",
                 queued);
        queued = 0;
    }
    while (queued-- && ui_queue_pop(&c->from_logic, &msg)) handle(c, msg);
    while (ui_queue_pop(&c->actor.queue, &msg)) handle(c, msg);
}

static uint32_t physical(uint32_t logical, uint32_t scale_120) {
    // Round half away from zero, as wp_fractional_scale_v1 specifies.
    return (uint32_t)(((uint64_t)logical * scale_120 + 60) / 120);
}

static bool surface_has_work(ui_core *c, const ui_core_surface *s) {
    const ui_scene_root *root = ui_scene_root_of(&c->scene, s->handle);
    return s->force_draw || s->resized_pending || s->config_changed ||
           (root && (root->dirty || root->animating));
}

// ---- Preparation (core/prepare.h) --------------------------------------------------------

static bool pending_ready(const struct ui_pending_cmds *p) {
    return !p->prep || ui_prep_ready(p->prep);
}

// Received buffers count as work only once the first can be applied.
static bool head_ready(const ui_core *c) {
    return c->pending_count && pending_ready(&c->pending[0]);
}

// Keeps each root's scale current: applied text adopts the shaping prepared at it.
static void sync_scales(ui_core *c) {
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        ui_core_surface *s = &c->surfaces[i];
        ui_scene_root *root = ui_handle_is_null(s->handle) ? NULL : ui_scene_root_of(&c->scene, s->handle);
        if (root && s->configured) root->scale = (float)s->config.scale_120 / 120.f;
    }
}

// The distinct scales of the configured surfaces: text is prepared at each of them.
static uint32_t surface_scales(const ui_core *c, float *out) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        const ui_core_surface *s = &c->surfaces[i];
        if (ui_handle_is_null(s->handle) || !s->configured) continue;
        float scale = (float)s->config.scale_120 / 120.f;
        bool seen = false;
        for (uint32_t k = 0; k < n && !seen; k++) seen = out[k] == scale;
        if (!seen) out[n++] = scale;
    }
    return n;
}

// The paragraph shows a source that comes from the node's set_text.
static bool shows_base(const ui_node *n) {
    const ui_text_source *src = n->text->source;
    return src == n->text_base || (src && src->shared == n->text_base);
}

// The head step (core/prepare.h) of pending[at]: with every earlier buffer applied, its
// fonts are registered, its text is prepared again where the values it was guessed with
// were wrong, and existing paragraphs whose values it changes (all of them after a font
// load) are shaped again, all before it applies. Returns true if that submitted work (the
// buffer then waits for it).
static bool head_step(ui_core *c, uint32_t at) {
    sync_scales(c);
    ui_scene *s = &c->scene;
    ui_prep_buffer *head = c->pending[at].prep;
    ui_prep_buffer **preps = malloc(c->pending_count * sizeof *preps);
    uint32_t prep_count = 0;
    for (uint32_t i = at; preps && i < c->pending_count; i++) {
        if (c->pending[i].prep) preps[prep_count++] = c->pending[i].prep;
    }
    // Without memory for the list, fonts are still registered; text left out is shaped on
    // the core when laid out.
    bool fonts = ui_prep_register_fonts(head, preps, prep_count), submitted = fonts;
    free(preps);
    ui_preview *pv = ui_preview_create(s, &(ui_preview_buffer){head->data, head->size}, 1);
    for (uint32_t i = 0; pv && i < head->scene.text_count; i++) {
        ui_scene_text_prep *tp = &head->scene.texts[i];
        ui_text_ctx ctx;
        if (!tp->op.source || !tp->scale_count || !ui_preview_ctx(pv, tp->op.node, &ctx, NULL)) continue;
        ui_text_resolved key = ui_text_ctx_key(&ctx, tp->op.source);
        if (!tp->shaped_source || !ui_text_source_shapes_as(tp->shaped_source, &key)) {
            ui_prep_restyle_item(head, i, &ctx);
            submitted = true;
        }
    }
    if (fonts || !pv || ui_preview_global(pv)) {
        ui_prep_paragraph *paras = malloc((s->handles.live ? s->handles.live : 1) * sizeof *paras);
        bool *derived = malloc((s->handles.live ? s->handles.live : 1) * sizeof *derived);
        uint32_t count = 0;
        for (uint32_t slot = 0; paras && derived && slot < s->handles.used; slot++) {
            ui_node *n = &s->nodes[slot];
            if (!s->handles.alive[slot] || !n->text || !n->text_base || !n->text->source) continue;
            ui_handle h = ui_scene_handle_of(s, slot);
            ui_text_source *target = n->text->source;
            bool made = false;
            if (pv) {
                ui_text_ctx ctx;
                if (ui_preview_sets_text(pv, slot) || !ui_preview_ctx(pv, h, &ctx, NULL)) continue;
                ui_text_resolved key = ui_text_ctx_key(&ctx, n->text_base);
                if (!shows_base(n) || !ui_text_source_shapes_as(n->text->source, &key)) {
                    if (ui_text_source_shapes_as(n->text_base, &key)) target = n->text_base;
                    else if (ui_text_source_derive(n->text_base, &key, &target) == UI_OK) made = true;
                    else continue; // out of memory: shaped on the core
                } else if (!fonts) {
                    continue; // unchanged
                }
            } else if (!fonts) {
                break; // out of memory for the preview: changed values are shaped on the core
            }
            derived[count] = made;
            paras[count++] = (ui_prep_paragraph){h, n->text_base, target, s->roots[n->root].scale};
        }
        if (paras && derived) ui_prep_reshape_paragraphs(head, paras, count); // references them
        submitted |= count > 0;
        for (uint32_t i = 0; derived && i < count; i++) {
            if (derived[i]) ui_text_source_release(paras[i].source);
        }
        free(paras);
        free(derived);
    }
    ui_preview_destroy(pv);
    ui_prep_head_done(head);
    return submitted;
}

// What the scene will make of the text of received buffer pending[at]: each set_text is
// prepared with the values its node will have once every buffer up to it has applied (the
// head step checks them).
static void guess_text(ui_core *c, uint32_t at) {
    ui_prep_buffer *b = c->pending[at].prep;
    if (!b->scene.text_count) return;
    ui_preview_buffer local[8], *bufs = at < 8 ? local : malloc((at + 1) * sizeof *bufs);
    if (!bufs) return; // shaped with set_text's own values; the head step corrects them
    for (uint32_t i = 0; i <= at; i++) bufs[i] = (ui_preview_buffer){c->pending[i].data, c->pending[i].size};
    ui_preview *pv = ui_preview_create(&c->scene, bufs, at + 1);
    if (bufs != local) free(bufs);
    for (uint32_t i = 0; pv && i < b->scene.text_count; i++) {
        ui_text_ctx ctx;
        if (ui_preview_ctx(pv, ui_wire_get_handle(b->items[i].op.payload), &ctx, NULL)) ui_prep_guess(b, i, &ctx);
    }
    ui_preview_destroy(pv);
}

// Starts received buffers once font discovery is ready, and runs the head step of a head
// buffer with fonts. Runs before and after a frame, never inside its apply phase, so when a
// head step happens relative to frames is the same under both schedulers.
static void advance_prep(ui_core *c) {
    if (!c->pending_count || !ui_preparer_discovery_ready(c->preparer)) return;
    float scales[UI_MAX_SURFACES];
    uint32_t n = surface_scales(c, scales);
    for (uint32_t i = 0; i < c->pending_count; i++) {
        ui_prep_buffer *b = c->pending[i].prep;
        if (b && !b->started) {
            guess_text(c, i);
            ui_prep_start(b, scales, n);
        }
    }
    ui_prep_buffer *head = c->pending[0].prep;
    if (head && ui_prep_idle(head) && ui_prep_needs_head_step(head)) head_step(c, 0);
}


// A prepared buffer was applied: its reshaped paragraphs are offered to their nodes, which
// adopt them as they resolve in this frame (the fonts it registered apply from now on).
static void prep_applied(ui_core *c, ui_prep_buffer *b) {
    if (b->fonts_changed) c->text.generation = b->generation;
    for (uint32_t i = 0; i < b->reshape_count; i++) {
        ui_prep_reshape *r = &b->reshapes[i];
        if (r->ok) ui_scene_offer_shaped(&c->scene, r->node, r->base, r->source, r->scale, &r->shaped);
    }
    if (b->fonts_changed) ui_scene_fonts_changed(&c->scene);
}

// Applies every buffer from the head on that is ready, in order; the first that isn't, and
// all after it, wait for a later frame.
static void apply_ready(ui_core *c, uint64_t now) {
    sync_scales(c);
    uint32_t done = 0;
    for (; done < c->pending_count; done++) {
        struct ui_pending_cmds *p = &c->pending[done];
        // A buffer that reaches the head here gets its head step now. If that gave the
        // workers something to do, applying stops: whether it is done by now depends on the
        // scheduler, and which frame applies it must not (single and threaded draw the same
        // frames). What it found to do is the same under both, since its text was prepared
        // for the scene as it is now (guess_text).
        if (p->prep && ui_prep_idle(p->prep) && ui_prep_needs_head_step(p->prep) && head_step(c, done)) break;
        if (!pending_ready(p)) break;
        if (p->deferred) {
            uint64_t waited = ui_monotonic_ns() - c->head_wait_since;
            c->stats.buffers_deferred++;
            if (waited > c->stats.max_head_wait_ns) c->stats.max_head_wait_ns = waited;
            if (waited > 100000000u)
                UI_DEBUG_REPORT(c->log, MODULE, "a command buffer waited %.1f ms for preparation",
                                (double)waited / 1e6);
            c->head_wait_since = 0;
        }
        uint64_t before = c->scene.stats.applied;
        c->applying = p->prep;
        if (ui_scene_apply_prepared(&c->scene, p->data, p->size, now, p->prep ? &p->prep->scene : NULL) != UI_OK)
            UI_ERROR(c->log, MODULE, "rejected a malformed command buffer");
        c->applying = NULL;
        c->stats.commands_applied += c->scene.stats.applied - before;
        if (p->prep) prep_applied(c, p->prep);
        if (p->prep) {
            // Its glyphs stay in the glyph cache until every surface has drawn once.
            c->unreleased_seq = p->prep->seq;
            c->unreleased_epoch = c->epoch + 1;
        }
        free_pending(c, done);
    }
    if (!done) return;
    memmove(c->pending, c->pending + done, (c->pending_count - done) * sizeof *c->pending);
    c->pending_count -= done;
}

// Notes a frame that the head buffer missed while preparing.
static void note_deferred(ui_core *c) {
    if (!c->pending_count || c->pending[0].deferred) return;
    c->pending[0].deferred = true;
    c->head_wait_since = ui_monotonic_ns();
}

// Glyphs prepared for the newest applied buffer may leave the glyph cache once every surface
// has drawn since it applied (or has nothing to draw).
static void release_prepared_glyphs(ui_core *c) {
    if (!c->unreleased_seq) return;
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        const ui_core_surface *s = &c->surfaces[i];
        if (ui_handle_is_null(s->handle)) continue;
        if (s->last_epoch < c->unreleased_epoch && surface_has_work(c, s)) return;
    }
    ui_glyph_cache_release(c->glyphs, c->unreleased_seq);
    c->unreleased_seq = 0;
}

// Queues the atlas changes made so far for the render actor. False only when memory ran out;
// the changes are then kept and retried.
static bool send_atlas_update(ui_core *c) {
    if (!c->unsent) {
        bool failed;
        c->unsent = ui_atlas_take_update(c->atlas, &failed);
        if (failed) return false;
    }
    if (!c->unsent) return true;
    ui_msg msg = {.type = UI_MSG_ATLAS_UPDATE, .ptr = c->unsent};
    if (ui_queue_push(&c->render->queue, &msg) != UI_OK) return false;
    c->unsent = NULL;
    return true;
}

// The oldest epoch among surfaces whose newest committed list render hasn't finished: no
// glyph used at or after it may be evicted.
static uint64_t protect_epoch(const ui_core *c) {
    uint64_t epoch = UINT64_MAX;
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        const ui_core_surface *s = &c->surfaces[i];
        if (ui_handle_is_null(s->handle) || s->last_done_frame >= s->next_frame_id) continue;
        if (s->last_epoch < epoch) epoch = s->last_epoch;
    }
    return epoch;
}

static void commit(ui_core *c, ui_core_surface *s, uint64_t now) {
    ui_scene_root *root = ui_scene_root_of(&c->scene, s->handle);
    if (root) root->dirty = false;
    s->force_draw = false;
    s->config_changed = false;
    // The drawn frame's presentation requests the next callback (ui_wl_pre_present).
    s->awaiting_frame = true;

    ui_display_list *dl = ui_mailbox_back(&s->mailbox);
    ui_display_list_reset(dl);
    dl->surface = s->handle;
    dl->frame_id = ++s->next_frame_id;
    dl->predicted_present_host_ns = s->predicted_host_ns;
    dl->anim_time_ns = now;
    dl->scale_120 = s->config.scale_120;
    dl->logical_width = (float)s->config.logical_width;
    dl->logical_height = (float)s->config.logical_height;
    dl->width = physical(s->config.logical_width, s->config.scale_120);
    dl->height = physical(s->config.logical_height, s->config.scale_120);
    dl->platform_token = s->config.token;
    memcpy(dl->clear, clear_color, sizeof dl->clear);
    dl->epoch = c->epoch;
    dl->text_contrast = c->text_contrast;
    dl->text_gamma = c->text_gamma;
    ui_scene_layout(&c->scene, s->handle, (float)s->config.logical_width,
                    (float)s->config.logical_height, (float)s->config.scale_120 / 120.f, &c->text, now);
    ui_scene_text text = {&c->text, c->atlas, &c->raster};
    if (ui_scene_build(&c->scene, s->handle, dl, &text) != UI_OK)
        UI_ERROR(c->log, MODULE, "display list incomplete (out of memory)");
    // The atlas changes this list needs go into the render inbox before the list is
    // published (docs/text.md): the inbox is FIFO and never replaces, the mailbox may.
    if (!send_atlas_update(c)) {
        UI_ERROR(c->log, MODULE, "out of memory sending glyphs; frame %llu not shown",
                 (unsigned long long)dl->frame_id);
        s->force_draw = true;
        s->awaiting_frame = false;
        s->next_frame_id--;
        return;
    }
    dl->atlas_seq = ui_atlas_seq(c->atlas);
    s->last_epoch = c->epoch;
    if (dl->rect_count)
        UI_TRACE_LOG(c->log, MODULE, "frame %llu t=%.3f s: first rect at %.1f,%.1f",
                     (unsigned long long)dl->frame_id, (double)now / 1e9, dl->rects[0].x,
                     dl->rects[0].y);
    if (ui_mailbox_publish(&s->mailbox)) c->stats.display_lists_replaced++;
    c->stats.frames_committed++;

    if (s->resized_pending) {
        s->resized_pending = false;
        ui_wire_surface_resized_put(&c->events, &(ui_wire_surface_resized){s->handle, (float)s->config.logical_width,
                                                                          (float)s->config.logical_height,
                                                                          s->config.scale_120});
    }
    ui_wire_frame_committed_put(&c->events, &(ui_wire_frame_committed){s->handle, dl->frame_id, now});
}

// True if the root's layout is what its surface's next frame would lay out: nothing under
// it changed, and the surface's size and scale are those it was laid out for.
static bool layout_current(ui_core *c, const ui_scene_root *root) {
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        const ui_core_surface *s = &c->surfaces[i];
        if (ui_handle_is_null(s->handle) || !ui_handle_eq(s->handle, root->surface)) continue;
        return !root->layout_dirty && root->layout_size[0] == (float)s->config.logical_width &&
               root->layout_size[1] == (float)s->config.logical_height &&
               root->layout_scale == (float)s->config.scale_120 / 120.f;
    }
    return false;
}

// Answers request_layout ops whose node's layout is current: right after the frame that
// applied them laid the surface out, or at once when nothing under the surface changed (a
// surface that didn't draw this step). The others wait for their surface's next frame;
// requests never make a frame themselves. Answers keep the order of the requests.
static void answer_requests(ui_core *c) {
    ui_scene *sc = &c->scene;
    if (!sc->request_count) return; // also keeps memmove away from a NULL array
    uint32_t kept = 0, i = 0;
    for (; i < sc->request_count; i++) {
        ui_scene_request q = sc->requests[i];
        ui_node *n = ui_scene_node(sc, q.node);
        if (n && !layout_current(c, &sc->roots[n->root])) {
            sc->requests[kept++] = q;
            continue;
        }
        float rects[4][4];
        ui_handle surface;
        bool live = ui_scene_measure(sc, q.node, rects, &surface);
        ui_wire_layout_measured ev = {.request_id = q.id, .result = (uint32_t)(live ? UI_OK : UI_ERR_STALE_HANDLE),
                                      .node = q.node, .surface = surface};
        ui_wire_rect *out[4] = {&ev.layout_in_parent, &ev.layout_in_surface, &ev.presentation_in_parent,
                                &ev.presentation_in_surface};
        for (int k = 0; k < 4; k++) *out[k] = (ui_wire_rect){rects[k][0], rects[k][1], rects[k][2], rects[k][3]};
        if (ui_wire_layout_measured_put(&c->events, &ev) != UI_OK) {
            UI_ERROR(c->log, MODULE, "out of memory answering request_layout");
            break; // retried at the next frame, in order
        }
    }
    memmove(sc->requests + kept, sc->requests + i, (sc->request_count - i) * sizeof *sc->requests);
    sc->request_count = kept + (sc->request_count - i);
}

static void frame(ui_core *c) {
    // One animation time for the whole frame: the earliest predicted present among the
    // surfaces drawing now. Per-surface sampling arrives with multi-monitor support.
    uint64_t host = UINT64_MAX;
    ui_core_surface *first = NULL;
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        ui_core_surface *s = &c->surfaces[i];
        if (ui_handle_is_null(s->handle) || !s->frame_ready) continue;
        if (!first) first = s;
        if (s->predicted_host_ns < host) host = s->predicted_host_ns;
    }
    if (!first) return;
    uint64_t now = ui_clock_at(&c->clock, host);
    if (now < c->last_anim_ns) now = c->last_anim_ns; // never run time backwards
    c->last_anim_ns = now;

    // Nothing to apply and no ready surface with work: every ready surface is idle, and
    // skipping costs no apply, animate or GPU work.
    bool busy = head_ready(c);
    for (uint32_t i = 0; i < UI_MAX_SURFACES && !busy; i++) {
        ui_core_surface *s = &c->surfaces[i];
        if (!ui_handle_is_null(s->handle) && s->frame_ready && surface_has_work(c, s)) busy = true;
    }
    ui_handle main = first->handle;
    uint64_t frame_id = first->next_frame_id + 1;
    if (busy) {
        for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
            ui_core_surface *s = &c->surfaces[i];
            if (!ui_handle_is_null(s->handle) && s->frame_ready)
                UI_HOOK(c->hooks, UI_TRACE_FRAME_BEGIN, 0, s->handle, s->next_frame_id + 1);
        }

        UI_HOOK_PHASE_BEGIN(c->hooks, UI_PHASE_APPLY, main, frame_id);
        apply_ready(c, now);
        c->stats.commands_dropped = c->scene.stats.dropped;
        c->stats.buffers_rejected = c->scene.stats.rejected;
        UI_HOOK_PHASE_END(c->hooks, UI_PHASE_APPLY, main, frame_id);

        UI_HOOK_PHASE_BEGIN(c->hooks, UI_PHASE_ANIMATE, main, frame_id);
        ui_scene_resolve(&c->scene, now);
        ui_scene_animate(&c->scene, now);
        UI_HOOK_PHASE_END(c->hooks, UI_PHASE_ANIMATE, main, frame_id);
    }

    UI_HOOK_PHASE_BEGIN(c->hooks, UI_PHASE_COMMIT, main, frame_id);
    bool drew = false;
    if (busy) ui_atlas_begin_frame(c->atlas, ++c->epoch, protect_epoch(c));
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        ui_core_surface *s = &c->surfaces[i];
        if (ui_handle_is_null(s->handle) || !s->frame_ready) continue;
        s->frame_ready = false;
        if (busy && surface_has_work(c, s)) {
            commit(c, s, now);
            drew = true;
        } else {
            // Idle: no commit, and no callback until work arrives (see wake_idle).
            s->awaiting_frame = false;
            c->stats.frames_skipped_idle++;
            if (busy) UI_HOOK(c->hooks, UI_TRACE_FRAME_END, 0, s->handle, s->next_frame_id + 1);
            UI_HOOK(c->hooks, UI_TRACE_FRAME_IDLE, 0, s->handle, s->next_frame_id);
        }
    }
    if (busy) ui_atlas_end_frame(c->atlas);
    if (busy) answer_requests(c);
    note_deferred(c);
    release_prepared_glyphs(c);
    c->stats.text_invalid_sequences = c->scene.stats.text_invalid;
    c->stats.text_nodes_with_children = c->scene.stats.text_nodes_with_children;
    ui_atlas_stats as;
    ui_atlas_get_stats(c->atlas, &as);
    c->stats.glyphs_rasterized = as.rasterized;
    c->stats.glyphs_evicted = as.evicted;
    c->stats.glyphs_rejected = as.rejected;
    c->stats.atlas_updates = as.updates;
    c->stats.atlas_pages = as.pages;
    c->stats.glyphs_cached = as.glyphs;
    c->stats.glyphs_rasterized_on_core = as.rasterized - as.from_cache;
    c->stats.text_shaped_on_core = c->text.shapes;
    ui_preparer_stats ps;
    ui_preparer_get_stats(c->preparer, &ps);
    c->stats.texts_prepared = ps.texts;
    c->stats.glyphs_prepared = ps.glyphs;
    c->stats.text_reshaped = ps.reshapes;
    UI_HOOK_PHASE_END(c->hooks, UI_PHASE_COMMIT, main, frame_id);
    if (drew) ui_signal_notify(&c->render->signal);
}

// Idle surfaces with new work ask the platform for a frame callback. Pending command buffers
// count as work for every surface: which one they touch is only known once applied, at a
// frame boundary.
static void wake_idle(ui_core *c) {
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        ui_core_surface *s = &c->surfaces[i];
        if (ui_handle_is_null(s->handle) || !s->configured) continue;
        if (s->frame_ready || s->awaiting_frame) continue;
        if (!head_ready(c) && !surface_has_work(c, s)) continue;
        s->awaiting_frame = true;
        c->platform->vt->surface_request_frame(s->platform);
    }
}

static void flush_events(ui_core *c) {
    if (!c->events.op_count || c->events_in_flight >= MAX_EVENTS_IN_FLIGHT) return;
    size_t size;
    uint8_t *bytes = ui_wire_buf_finish(&c->events, &size);
    if (!bytes) return;
#ifdef UI_WIRE_PROOF
    ui_wire_proof_record("ev", bytes, size);
#endif
    ui_msg msg = {.type = UI_MSG_EVENT_BUFFER, .ptr = bytes, .size = size};
    // Cannot be full: at most MAX_EVENTS_IN_FLIGHT buffers are ever queued.
    if (ui_queue_push(&c->logic->queue, &msg) != UI_OK) {
        UI_ERROR(c->log, MODULE, "logic queue full; dropped events");
        free(bytes);
        return;
    }
    c->events_in_flight++;
}

void ui_core_step(void *self) {
    ui_core *c = self;
    drain(c);
    advance_prep(c);
    fire_timers(c);
    frame(c);
    advance_prep(c);
    wake_idle(c);
    flush_events(c);
}

void ui_core_receive(ui_core *c) {
    drain(c);
    advance_prep(c);
}

bool ui_core_preparing(const ui_core *c) {
    if (!ui_preparer_discovery_ready(c->preparer)) return true;
    for (uint32_t i = 0; i < c->pending_count; i++) {
        const ui_prep_buffer *b = c->pending[i].prep;
        if (b && (!b->started || !ui_prep_idle(b))) return true;
    }
    // A head that still needs its head step gets it at the end of this core step.
    return c->pending_count && c->pending[0].prep && ui_prep_needs_head_step(c->pending[0].prep);
}

uint64_t ui_core_wait_timeout_ns(ui_core *c) {
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        const ui_core_surface *s = &c->surfaces[i];
        if (!ui_handle_is_null(s->handle) && s->frame_ready) return 0;
    }
    if (ui_queue_count(&c->from_logic) || ui_queue_count(&c->actor.queue)) return 0;
    if (c->events.op_count && c->events_in_flight < MAX_EVENTS_IN_FLIGHT) return 0;
    uint64_t next = UINT64_MAX;
    for (uint32_t i = 0; i < c->timer_count; i++) {
        if (c->timers[i].deadline_ns < next) next = c->timers[i].deadline_ns;
    }
    if (next == UINT64_MAX) return UINT64_MAX;
    uint64_t at = ui_clock_host_at(&c->clock, next), now = ui_clock_host_now(&c->clock);
    if (at == UINT64_MAX) return UINT64_MAX; // paused: only stepping moves the clock
    return at > now ? at - now : 0;
}

bool ui_core_frames_done(const ui_core *c) {
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        const ui_core_surface *s = &c->surfaces[i];
        if (!ui_handle_is_null(s->handle) && s->last_done_frame < s->next_frame_id) return false;
    }
    return true;
}

bool ui_core_idle(const ui_core *c) {
    if (c->events_in_flight || c->events.op_count || ui_core_preparing(c)) return false;
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        const ui_core_surface *s = &c->surfaces[i];
        if (ui_handle_is_null(s->handle)) continue;
        if (s->last_done_frame < s->next_frame_id) return false;
    }
    return true;
}
