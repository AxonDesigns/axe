// The app's fonts: faces loaded from files or found through the platform's font discovery
// (Fontconfig on Linux), family matching, per-cluster fallback, and the cache of sized font
// instances. Owned by the core.
//
// Thread-safe: the core and the worker threads that prepare text use it concurrently. Faces
// and fonts are immutable once created and never freed before the registry, so pointers it
// returns stay valid. Face lookups by id take no lock; matching, fallback and instance
// creation take a mutex, which is released around Fontconfig queries and file loading.
//
// Registered faces (font files, load_font) are kept in registration order, which only the
// core changes; faces found by discovery are cached separately, so fallback order never
// depends on which thread discovered what first.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "runtime/common.h"
#include "text/hb/hb_font.h"

#define UI_NO_FACE UINT32_MAX

typedef enum ui_font_style {
    UI_FONT_NORMAL = 0,
    UI_FONT_ITALIC = 1,
    UI_FONT_OBLIQUE = 2,
} ui_font_style;

// Identifies one font instance; part of the glyph cache key.
typedef struct ui_font_key {
    uint32_t face;     // registry face id
    uint32_t instance; // interned variation coordinates (0: the face's default instance)
    uint32_t size_q;   // physical size in 1/64 px
} ui_font_key;

typedef struct ui_font_registry ui_font_registry;

typedef struct ui_font_registry_desc {
    const ui_logger *log;
    bool system_fonts;            // use the platform's installed fonts for families/fallback
    const char *const *font_dirs; // extra directories for font discovery (may be NULL)
    uint32_t font_dir_count;
    // Leave discovery off until ui_font_registry_start_discovery runs (on a worker: loading
    // the system configuration can take tens of milliseconds).
    bool defer_discovery;
} ui_font_registry_desc;

ui_result ui_font_registry_create(const ui_font_registry_desc *desc, ui_font_registry **out);
void ui_font_registry_destroy(ui_font_registry *r);

// Creates the discovery backend of a registry made with defer_discovery. Blocking; call it
// once, from any thread. Matching treats discovery as absent until it completes.
void ui_font_registry_start_discovery(ui_font_registry *r);
// True while discovery was asked for but ui_font_registry_start_discovery hasn't finished.
bool ui_font_registry_discovery_pending(const ui_font_registry *r);

// Loads a face and registers it; families are matched by its typographic family name.
ui_result ui_font_registry_add_file(ui_font_registry *r, const char *path, uint32_t index,
                                    uint32_t *face_id);
// Registers a face loaded elsewhere (on a worker); the registry takes ownership. A file that
// is already registered keeps its face and `face` is destroyed. *changed (may be NULL) tells
// whether the set of registered faces grew, which bumps the generation.
ui_result ui_font_registry_register(ui_font_registry *r, const char *path, uint32_t index,
                                    ui_face *face, uint32_t *face_id, bool *changed);
// Increases whenever the set of registered faces changes: shaping done before may now
// match or fall back differently.
uint64_t ui_font_registry_generation(const ui_font_registry *r);
// Faces known so far (registered and discovered).
uint32_t ui_font_registry_face_count(const ui_font_registry *r);
uint32_t ui_font_registry_registered_count(ui_font_registry *r);
ui_face *ui_font_registry_face(const ui_font_registry *r, uint32_t face_id);

// The face for a CSS-style family list ("Noto Sans, \"My Font\", sans-serif") at a weight
// (1..1000) and style. Each family is tried in order: registered faces first, then font
// discovery. Without a match, the default family (sans-serif from discovery, or the first
// registered face's family). UI_NO_FACE only when there are no fonts at all.
uint32_t ui_font_registry_match(ui_font_registry *r, const char *families, uint32_t len,
                                uint16_t weight, ui_font_style style);

// The face to render one grapheme cluster (cps[0..n)) with: `primary` if it covers every
// code point (default-ignorables excepted), else the first that does among the family
// list, registered faces in registration order, and font discovery. Falls back to
// `primary`, which then draws .notdef.
uint32_t ui_font_registry_resolve(ui_font_registry *r, uint32_t primary, const char *families,
                                  uint32_t len, uint16_t weight, ui_font_style style,
                                  const uint32_t *cps, uint32_t n);

// True if `face` covers every code point in cps (default-ignorables excepted).
bool ui_font_registry_covers(const ui_font_registry *r, uint32_t face, const uint32_t *cps,
                             uint32_t n);

// The cached instance of a face at a physical size for a weight and style: variable axes
// (wght, ital, slnt) are set from them. NULL only on allocation failure.
ui_font *ui_font_registry_font(ui_font_registry *r, uint32_t face, float size_px,
                               uint16_t weight, ui_font_style style, ui_font_key *key);
// The instance for a key previously returned by ui_font_registry_font.
ui_font *ui_font_registry_font_by_key(ui_font_registry *r, ui_font_key key);
