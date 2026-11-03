#include "text/glyph_cache.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "runtime/common.h"

#define SHARDS 16u

enum { SLOT_EMPTY = 0, SLOT_LIVE, SLOT_TOMBSTONE };

typedef struct slot {
    ui_glyph_key key;
    uint8_t state;
    bool failed;          // could not be rasterized
    int16_t left, top;
    uint16_t width, height;
    uint64_t seq;         // protected until the core releases this sequence number
    uint64_t used;        // shard tick of the last use
    uint8_t *pixels;
} slot;

typedef struct shard {
    mtx_t lock;
    slot *slots;
    uint32_t capacity, live, tombstones;
    size_t bytes;
    uint64_t tick;
    uint64_t stored, evicted;
} shard;

struct ui_glyph_cache {
    size_t budget;        // per shard
    _Atomic uint64_t released;
    shard shards[SHARDS];
};

static uint32_t hash_key(ui_glyph_key k) {
    uint32_t h = k.font.face * 0x9E3779B1u;
    h ^= k.font.instance * 0x85EBCA77u;
    h ^= k.font.size_q * 0xC2B2AE3Du;
    h ^= k.glyph * 0x27D4EB2Fu;
    h ^= k.bin * 0x165667B1u;
    return h ^ (h >> 16);
}

static bool same(ui_glyph_key a, ui_glyph_key b) {
    return a.font.face == b.font.face && a.font.instance == b.font.instance &&
           a.font.size_q == b.font.size_q && a.glyph == b.glyph && a.bin == b.bin;
}

ui_result ui_glyph_cache_create(size_t budget, ui_glyph_cache **out) {
    ui_glyph_cache *c = calloc(1, sizeof *c);
    if (!c) return UI_ERR_OUT_OF_MEMORY;
    c->budget = (budget ? budget : UI_GLYPH_CACHE_DEFAULT_BYTES) / SHARDS;
    atomic_init(&c->released, 0u);
    for (uint32_t i = 0; i < SHARDS; i++) {
        if (mtx_init(&c->shards[i].lock, mtx_plain) != thrd_success) {
            for (uint32_t j = 0; j < i; j++) mtx_destroy(&c->shards[j].lock);
            free(c);
            return UI_ERR_OUT_OF_MEMORY;
        }
    }
    *out = c;
    return UI_OK;
}

void ui_glyph_cache_destroy(ui_glyph_cache *c) {
    if (!c) return;
    for (uint32_t i = 0; i < SHARDS; i++) {
        shard *s = &c->shards[i];
        for (uint32_t k = 0; k < s->capacity; k++) {
            if (s->slots[k].state == SLOT_LIVE) free(s->slots[k].pixels);
        }
        free(s->slots);
        mtx_destroy(&s->lock);
    }
    free(c);
}

static shard *shard_of(ui_glyph_cache *c, uint32_t h) { return &c->shards[h % SHARDS]; }

// The slot holding `key`, or the slot to insert it into (*found false). Capacity > 0.
static slot *probe(shard *s, ui_glyph_key key, uint32_t h, bool *found) {
    uint32_t mask = s->capacity - 1;
    slot *reuse = NULL;
    for (uint32_t i = (h / SHARDS) & mask;; i = (i + 1) & mask) {
        slot *e = &s->slots[i];
        if (e->state == SLOT_EMPTY) {
            *found = false;
            return reuse ? reuse : e;
        }
        if (e->state == SLOT_TOMBSTONE) {
            if (!reuse) reuse = e;
        } else if (same(e->key, key)) {
            *found = true;
            return e;
        }
    }
}

static bool rehash(shard *s, uint32_t capacity) {
    slot *old = s->slots;
    uint32_t old_cap = s->capacity;
    s->slots = calloc(capacity, sizeof *s->slots);
    if (!s->slots) {
        s->slots = old;
        return false;
    }
    s->capacity = capacity;
    s->tombstones = 0;
    for (uint32_t i = 0; i < old_cap; i++) {
        if (old[i].state != SLOT_LIVE) continue;
        bool found;
        *probe(s, old[i].key, hash_key(old[i].key), &found) = old[i];
    }
    free(old);
    return true;
}

// Room for one more entry: rehashes at 75% load (doubling unless mostly tombstones).
static bool reserve(shard *s) {
    if ((s->live + s->tombstones + 1) * 4 <= s->capacity * 3) return true;
    uint32_t cap = !s->capacity ? 64
                   : s->live * 2 + 2 > s->capacity / 2 ? s->capacity * 2
                                                       : s->capacity;
    return rehash(s, cap);
}

static int by_use(const void *a, const void *b) {
    const slot *x = *(slot *const *)a, *y = *(slot *const *)b;
    return x->used < y->used ? -1 : x->used > y->used;
}

// Evicts unprotected entries, least recently used first, until the shard is at 7/8 of its
// budget (or nothing more may go).
static void trim(ui_glyph_cache *c, shard *s) {
    if (s->bytes <= c->budget) return;
    uint64_t released = atomic_load_explicit(&c->released, memory_order_acquire);
    slot **cands = malloc(s->live * sizeof *cands);
    if (!cands) return;
    uint32_t n = 0;
    for (uint32_t i = 0; i < s->capacity; i++) {
        if (s->slots[i].state == SLOT_LIVE && s->slots[i].seq <= released) cands[n++] = &s->slots[i];
    }
    qsort(cands, n, sizeof *cands, by_use);
    for (uint32_t i = 0; i < n && s->bytes > c->budget / 8 * 7; i++) {
        slot *e = cands[i];
        s->bytes -= (size_t)e->width * e->height;
        free(e->pixels);
        e->pixels = NULL;
        e->state = SLOT_TOMBSTONE;
        s->live--;
        s->tombstones++;
        s->evicted++;
    }
    free(cands);
}

bool ui_glyph_cache_protect(ui_glyph_cache *c, ui_glyph_key key, uint64_t seq) {
    uint32_t h = hash_key(key);
    shard *s = shard_of(c, h);
    mtx_lock(&s->lock);
    bool found = false;
    if (s->capacity) {
        slot *e = probe(s, key, h, &found);
        if (found && e->seq < seq) e->seq = seq;
    }
    mtx_unlock(&s->lock);
    return found;
}

ui_result ui_glyph_cache_put(ui_glyph_cache *c, ui_glyph_key key, const ui_bitmap *bm,
                             uint64_t seq) {
    uint32_t h = hash_key(key);
    shard *s = shard_of(c, h);
    size_t bytes = bm ? (size_t)bm->width * bm->height : 0;
    uint8_t *pixels = NULL;
    if (bytes && !(pixels = malloc(bytes))) return UI_ERR_OUT_OF_MEMORY;
    if (bytes) memcpy(pixels, bm->pixels, bytes);
    mtx_lock(&s->lock);
    ui_result res = UI_OK;
    if (!reserve(s)) {
        res = UI_ERR_OUT_OF_MEMORY;
    } else {
        bool found;
        slot *e = probe(s, key, h, &found);
        if (found) {
            if (e->seq < seq) e->seq = seq;
            free(pixels);
        } else {
            if (e->state == SLOT_TOMBSTONE) s->tombstones--;
            *e = (slot){.key = key, .state = SLOT_LIVE, .failed = !bm, .seq = seq,
                        .used = ++s->tick, .pixels = pixels};
            if (bm) {
                e->left = (int16_t)bm->left;
                e->top = (int16_t)bm->top;
                e->width = (uint16_t)bm->width;
                e->height = (uint16_t)bm->height;
            }
            s->live++;
            s->bytes += bytes;
            s->stored++;
            trim(c, s);
        }
    }
    mtx_unlock(&s->lock);
    if (res != UI_OK) free(pixels);
    return res;
}

bool ui_glyph_cache_with(ui_glyph_cache *c, ui_glyph_key key,
                         void (*fn)(void *user, const ui_bitmap *bm), void *user) {
    uint32_t h = hash_key(key);
    shard *s = shard_of(c, h);
    mtx_lock(&s->lock);
    bool found = false;
    if (s->capacity) {
        slot *e = probe(s, key, h, &found);
        if (found) {
            e->used = ++s->tick;
            ui_bitmap bm = {e->left, e->top, e->width, e->height, e->pixels};
            fn(user, e->failed ? NULL : &bm);
        }
    }
    mtx_unlock(&s->lock);
    return found;
}

void ui_glyph_cache_release(ui_glyph_cache *c, uint64_t seq) {
    if (seq > atomic_load_explicit(&c->released, memory_order_relaxed))
        atomic_store_explicit(&c->released, seq, memory_order_release);
}

void ui_glyph_cache_get_stats(ui_glyph_cache *c, ui_glyph_cache_stats *out) {
    memset(out, 0, sizeof *out);
    for (uint32_t i = 0; i < SHARDS; i++) {
        shard *s = &c->shards[i];
        mtx_lock(&s->lock);
        out->entries += s->live;
        out->bytes += s->bytes;
        out->stored += s->stored;
        out->evicted += s->evicted;
        mtx_unlock(&s->lock);
    }
}
