#ifndef CURSOR_H
#define CURSOR_H

#include <stdbool.h>
#include <stddef.h>

#include "table.h"

typedef struct {
    const Table *table;
    size_t       next_row;
} Cursor;

void cursor_init(Cursor *c, const Table *t);

/* Advances past tombstoned rows and yields the next live row in storage
 * order, as an opaque RowRef - the executor never sees a raw row number
 * (see index.h's RowRef doc comment). Returns false at end of table. */
bool cursor_next(Cursor *c, RowRef *out_row);

#endif
