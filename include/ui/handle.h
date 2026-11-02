// Generational handles: how every core object is referenced across the C API and the wire.
//
// A handle is an index into a typed table plus a generation. The top 4 bits of the index
// carry the handle kind, so a surface handle can never be mistaken for a node handle.
// Generation 0 is never valid, so a zeroed handle is the null handle.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ui/result.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ui_handle {
    uint32_t index;      // kind << 28 | slot
    uint32_t generation;
} ui_handle;

typedef enum ui_handle_kind {
    UI_HANDLE_NODE = 0,    // allocated by the logic side, mirrored by the core
    UI_HANDLE_SURFACE = 1, // allocated by the core
} ui_handle_kind;

#define UI_HANDLE_SLOT_BITS 28u
#define UI_HANDLE_SLOT_MASK ((1u << UI_HANDLE_SLOT_BITS) - 1u)
// Hard cap on slots per table; indices above it are rejected as malformed.
#define UI_HANDLE_MAX_SLOTS (1u << 20)

static inline ui_handle_kind ui_handle_kind_of(ui_handle h) {
    return (ui_handle_kind)(h.index >> UI_HANDLE_SLOT_BITS);
}
static inline uint32_t ui_handle_slot(ui_handle h) { return h.index & UI_HANDLE_SLOT_MASK; }
static inline bool ui_handle_is_null(ui_handle h) { return h.generation == 0; }
static inline bool ui_handle_eq(ui_handle a, ui_handle b) {
    return a.index == b.index && a.generation == b.generation;
}

// Client-side allocator for node handles. The logic side owns node identity: it allocates
// handles itself (no round trip to the core) and the core validates every handle it
// receives against its mirror of the same generational scheme.
typedef struct ui_handle_allocator ui_handle_allocator;

// Thread: any; the allocator is then used only by the creating thread.
// Ownership: caller owns *out and frees it with ui_handle_allocator_destroy.
ui_result ui_handle_allocator_create(ui_handle_allocator **out);
void ui_handle_allocator_destroy(ui_handle_allocator *allocator);

// Allocates a node handle whose generation is newer than any earlier use of its slot.
ui_result ui_handle_alloc(ui_handle_allocator *allocator, ui_handle *out);
// Returns the slot for reuse; the next handle in this slot gets a new generation.
// Returns UI_ERR_STALE_HANDLE if the handle is not currently allocated.
ui_result ui_handle_free(ui_handle_allocator *allocator, ui_handle handle);

#ifdef __cplusplus
}
#endif
