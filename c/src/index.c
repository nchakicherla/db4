#include "index.h"

#define INITIAL_CAP 8

static bool needs_grow(const RowIndex *idx) {
    return idx->cap == 0 || idx->count >= idx->cap / 4 * 3;
}

static void raw_insert(RowIndex *idx, uint64_t hash, size_t row) {
    size_t mask = idx->cap - 1;
    size_t i = hash & mask;
    while (idx->slots[i].occupied) i = (i + 1) & mask;
    idx->slots[i].hash     = hash;
    idx->slots[i].row      = row;
    idx->slots[i].occupied = true;
}

static bool grow(RowIndex *idx, Arena *a) {
    size_t new_cap = idx->cap ? arena_checked_mul(a, idx->cap, 2) : INITIAL_CAP;

    RowIndex grown = {0};
    grown.slots = arena_zalloc(a, arena_checked_mul(a, new_cap, sizeof(RowIndexSlot)), _Alignof(RowIndexSlot));
    if (!grown.slots) return false;
    grown.cap   = new_cap;
    grown.count = idx->count;

    for (size_t i = 0; i < idx->cap; i++)
        if (idx->slots[i].occupied) raw_insert(&grown, idx->slots[i].hash, idx->slots[i].row);

    *idx = grown;
    return true;
}

void row_index_insert(RowIndex *idx, Arena *a, uint64_t hash, size_t row) {
    if (needs_grow(idx) && !grow(idx, a)) {
        idx->degraded = true;
        return;
    }
    raw_insert(idx, hash, row);
    idx->count++;
}

bool row_index_usable(const RowIndex *idx) {
    return !idx->degraded;
}

void row_index_find(const RowIndex *idx, uint64_t hash, void *ctx, bool (*visit)(void *ctx, size_t row)) {
    if (idx->cap == 0) return;

    size_t mask = idx->cap - 1;
    size_t i = hash & mask;
    for (size_t probes = 0; probes < idx->cap; probes++) {
        if (!idx->slots[i].occupied) return;
        if (idx->slots[i].hash == hash)
            if (!visit(ctx, idx->slots[i].row)) return;
        i = (i + 1) & mask;
    }
}

void row_index_clear(RowIndex *idx) {
    *idx = (RowIndex){0};
}
