// What a command buffer that hasn't applied yet would make of the scene's text values:
// for any node, the text context (core/scene.h) it would have once the buffer applies.
// Preparation uses it to shape text with the values it will have, on workers, before the
// buffer applies (docs/text.md, "Preparation").
//
// It reads the scene through an overlay of the buffer's ops that can change text values:
// node creation, destruction and moves, style lists, overrides, states, set_text, style
// definitions and tokens. Other ops don't change text values and are ignored. The scene
// must not change while a preview is in use.
//
// Thread: the core.
#pragma once

#include "core/scene.h"

typedef struct ui_preview ui_preview;

// Previews `count` buffers applied in order after the scene (the pending buffers up to the
// one of interest: their effects come first). NULL when out of memory. Framing must be valid.
typedef struct ui_preview_buffer {
    const uint8_t *data;
    size_t size;
} ui_preview_buffer;
ui_preview *ui_preview_create(const ui_scene *s, const ui_preview_buffer *buffers, uint32_t count);
void ui_preview_destroy(ui_preview *p);

// The node's text context after the buffers, and its set_text then (NULL: none; for a
// set_text of the buffers, its paragraph values without the text). False if the node
// wouldn't exist, or out of memory. Both stay valid until the preview is destroyed.
bool ui_preview_ctx(ui_preview *p, ui_handle node, ui_text_ctx *out, const ui_text_source **base);

// True if the buffers can change the text values of nodes besides those they set text on
// (they define styles, set tokens, move nodes, or change the styles, overrides or state of
// a node in a way that touches text values).
bool ui_preview_global(const ui_preview *p);

// True if the buffers set text on the node's slot.
bool ui_preview_sets_text(const ui_preview *p, uint32_t slot);
