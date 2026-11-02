#include "runtime/handle_table.h"

#include <string.h>

#include "runtime/common.h"

void ui_handle_table_init(ui_handle_table *t, ui_handle_kind kind) {
    memset(t, 0, sizeof *t);
    t->kind = kind;
    t->free_head = UINT32_MAX;
}

void ui_handle_table_destroy(ui_handle_table *t) {
    free(t->generations);
    free(t->alive);
    free(t->next_free);
    ui_handle_table_init(t, t->kind);
}

static ui_result reserve(ui_handle_table *t, uint32_t slots) {
    if (slots <= t->capacity) return UI_OK;
    if (slots > UI_HANDLE_MAX_SLOTS) return UI_ERR_FULL;
    uint32_t cap = t->capacity ? t->capacity : 16;
    while (cap < slots) cap *= 2;
    if (cap > UI_HANDLE_MAX_SLOTS) cap = UI_HANDLE_MAX_SLOTS;
    uint32_t *gens = realloc(t->generations, cap * sizeof *gens);
    if (!gens) return UI_ERR_OUT_OF_MEMORY;
    t->generations = gens;
    uint8_t *alive = realloc(t->alive, cap);
    if (!alive) return UI_ERR_OUT_OF_MEMORY;
    t->alive = alive;
    uint32_t *next = realloc(t->next_free, cap * sizeof *next);
    if (!next) return UI_ERR_OUT_OF_MEMORY;
    t->next_free = next;
    memset(gens + t->capacity, 0, (cap - t->capacity) * sizeof *gens);
    memset(alive + t->capacity, 0, cap - t->capacity);
    t->capacity = cap;
    return UI_OK;
}

static ui_handle make(const ui_handle_table *t, uint32_t slot) {
    return (ui_handle){((uint32_t)t->kind << UI_HANDLE_SLOT_BITS) | slot, t->generations[slot]};
}

ui_result ui_handle_table_alloc(ui_handle_table *t, ui_handle *out) {
    uint32_t slot;
    if (t->free_head != UINT32_MAX) {
        slot = t->free_head;
        t->free_head = t->next_free[slot];
    } else {
        ui_result r = reserve(t, t->used + 1);
        if (r != UI_OK) return r;
        slot = t->used++;
    }
    // Generation 0 is the null handle; skip it when the counter wraps.
    if (++t->generations[slot] == 0) t->generations[slot] = 1;
    t->alive[slot] = 1;
    t->live++;
    *out = make(t, slot);
    return UI_OK;
}

ui_result ui_handle_table_claim(ui_handle_table *t, ui_handle h) {
    if (ui_handle_kind_of(h) != t->kind || ui_handle_is_null(h)) return UI_ERR_MALFORMED;
    uint32_t slot = ui_handle_slot(h);
    if (slot >= UI_HANDLE_MAX_SLOTS) return UI_ERR_MALFORMED;
    ui_result r = reserve(t, slot + 1);
    if (r != UI_OK) return r;
    if (t->alive[slot] || h.generation <= t->generations[slot]) return UI_ERR_STALE_HANDLE;
    if (slot >= t->used) t->used = slot + 1;
    t->generations[slot] = h.generation;
    t->alive[slot] = 1;
    t->live++;
    return UI_OK;
}

bool ui_handle_table_valid(const ui_handle_table *t, ui_handle h) {
    if (ui_handle_kind_of(h) != t->kind || ui_handle_is_null(h)) return false;
    uint32_t slot = ui_handle_slot(h);
    return slot < t->used && t->alive[slot] && t->generations[slot] == h.generation;
}

ui_result ui_handle_table_release(ui_handle_table *t, ui_handle h) {
    if (!ui_handle_table_valid(t, h)) return UI_ERR_STALE_HANDLE;
    uint32_t slot = ui_handle_slot(h);
    t->alive[slot] = 0;
    t->live--;
    t->next_free[slot] = t->free_head;
    t->free_head = slot;
    return UI_OK;
}

// ---- Public client-side allocator --------------------------------------------------------

struct ui_handle_allocator {
    ui_handle_table table;
};

ui_result ui_handle_allocator_create(ui_handle_allocator **out) {
    ui_handle_allocator *a = malloc(sizeof *a);
    if (!a) return UI_ERR_OUT_OF_MEMORY;
    ui_handle_table_init(&a->table, UI_HANDLE_NODE);
    *out = a;
    return UI_OK;
}

void ui_handle_allocator_destroy(ui_handle_allocator *allocator) {
    if (!allocator) return;
    ui_handle_table_destroy(&allocator->table);
    free(allocator);
}

ui_result ui_handle_alloc(ui_handle_allocator *allocator, ui_handle *out) {
    return ui_handle_table_alloc(&allocator->table, out);
}

ui_result ui_handle_free(ui_handle_allocator *allocator, ui_handle handle) {
    return ui_handle_table_release(&allocator->table, handle);
}
