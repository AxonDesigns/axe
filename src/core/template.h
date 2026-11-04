#pragma once

// Templates (docs/templates.md): static subtrees defined once (define_template) and
// instantiated with one op (instantiate_template). An instance is exactly its template's ops
// with the instance's handles: the core expands each command buffer's instances into those
// ops when it receives the buffer, before preparing or applying anything, so the scene, text
// preparation and every check downstream see plain ops.
//
// Definitions follow receipt order, which is the order buffers apply in. Redefining an id
// replaces the definition for later instances; existing instances are plain nodes and don't
// change. destroy_template forgets one.

#include <stddef.h>
#include <stdint.h>

#include "runtime/common.h"

typedef struct ui_templates ui_templates;

typedef struct ui_template_stats {
    uint64_t defined, destroyed, instances, dropped;
} ui_template_stats;

ui_result ui_templates_create(const ui_logger *log, ui_templates **out);
void ui_templates_destroy(ui_templates *t);

// Processes a command buffer's define_template, destroy_template and instantiate_template ops
// in order. If it holds instances, `*out` is a new buffer (malloc'd, `*out_size` bytes) with
// each instance replaced by its ops; otherwise `*out` is NULL and the buffer is used as it is.
// Definitions and destructions stay in the buffer (the scene ignores them). An instance of an
// unknown template, or with as many handles as its template has nodes, is dropped. A buffer
// that isn't well framed is left alone (it is rejected when applied). UI_ERR_OUT_OF_MEMORY
// leaves `*out` NULL.
ui_result ui_templates_expand(ui_templates *t, const uint8_t *data, size_t size, uint8_t **out, size_t *out_size);

ui_template_stats ui_templates_stats(const ui_templates *t);

// Local handles in a template's ops: slot = the node's index in the template, generation 1.
#define UI_TEMPLATE_GENERATION 1u
#define UI_TEMPLATE_MAX_NODES 1024u
