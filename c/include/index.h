#ifndef INDEX_H
#define INDEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"

typedef struct {
    uint64_t hash;
    size_t   row;
    bool     occupied;
} RowIndexSlot;

typedef struct {
    RowIndexSlot *slots;
    size_t cap;
    size_t count;
    bool   degraded;
} RowIndex;

void row_index_insert(RowIndex *idx, Arena *a, uint64_t hash, size_t row);

bool row_index_usable(const RowIndex *idx);

void row_index_find(const RowIndex *idx, uint64_t hash, void *ctx, bool (*visit)(void *ctx, size_t row));

void row_index_clear(RowIndex *idx);

#endif
