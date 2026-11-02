// Generational slot table. Stores only liveness and generations; callers keep the object
// data in parallel arrays indexed by slot. Not thread-safe: each table has one owner.
#pragma once

#include <stdint.h>

#include "ui/handle.h"

typedef struct ui_handle_table {
    ui_handle_kind kind;
    uint32_t *generations; // last generation issued or claimed per slot
    uint8_t *alive;
    uint32_t *next_free;   // free list links (allocating tables only)
    uint32_t capacity;     // allocated slots
    uint32_t used;         // slots ever touched (high-water mark)
    uint32_t free_head;    // UINT32_MAX when empty
    uint32_t live;
} ui_handle_table;

void ui_handle_table_init(ui_handle_table *t, ui_handle_kind kind);
void ui_handle_table_destroy(ui_handle_table *t);

// Allocating mode: issues a fresh handle (owner-allocated objects).
ui_result ui_handle_table_alloc(ui_handle_table *t, ui_handle *out);

// Mirroring mode: accepts a handle allocated elsewhere. The slot must be free and the
// generation newer than the slot's last one, otherwise UI_ERR_STALE_HANDLE. Wrong kind or a
// slot beyond UI_HANDLE_MAX_SLOTS gives UI_ERR_MALFORMED. Do not mix with alloc on one table.
ui_result ui_handle_table_claim(ui_handle_table *t, ui_handle h);

// True if `h` has this table's kind and names a live object of the same generation.
bool ui_handle_table_valid(const ui_handle_table *t, ui_handle h);

// Releases a live handle. Returns UI_ERR_STALE_HANDLE if it is not live.
ui_result ui_handle_table_release(ui_handle_table *t, ui_handle h);

// Slot capacity; parallel arrays must be grown to this after alloc/claim.
static inline uint32_t ui_handle_table_capacity(const ui_handle_table *t) { return t->capacity; }
