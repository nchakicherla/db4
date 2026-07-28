#ifndef INDEX_H
#define INDEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"

/* An opaque handle to a live row inside some Table. Deliberately not an
 * integer as far as anything outside table.c/cursor.c/index.c is
 * concerned - it lets every layer above the storage engine (the executor,
 * the transaction manager, WAL replay, the CSV loader) carry a row's
 * identity around without assuming anything about how the storage engine
 * locates it by that identity. A dense row number into a flat array is
 * today's implementation; a future b-tree's own addressing scheme is a
 * drop-in replacement here, not a ripple through every caller (see
 * docs/persistence_progression.md, Stage 0). row_ref()/row_ref_raw() are
 * the only sanctioned way to cross that boundary - every call to either
 * outside table.c/cursor.c/index.c is a seam that still assumes today's
 * row-number representation. */
typedef struct {
    size_t v;
} RowRef;

#define ROW_REF_INVALID ((RowRef){ SIZE_MAX })

static inline RowRef row_ref(size_t v) { return (RowRef){ v }; }
static inline size_t row_ref_raw(RowRef r) { return r.v; }
static inline bool   row_ref_valid(RowRef r) { return r.v != SIZE_MAX; }
static inline bool   row_ref_eq(RowRef a, RowRef b) { return a.v == b.v; }

typedef struct {
    uint64_t hash;
    RowRef   row;
    bool     occupied;
} RowIndexSlot;

typedef struct {
    RowIndexSlot *slots;
    size_t cap;
    size_t count;
    bool   degraded;
} RowIndex;

void row_index_insert(RowIndex *idx, Arena *a, uint64_t hash, RowRef row);

bool row_index_usable(const RowIndex *idx);

void row_index_find(const RowIndex *idx, uint64_t hash, void *ctx, bool (*visit)(void *ctx, RowRef row));

void row_index_clear(RowIndex *idx);

#endif
