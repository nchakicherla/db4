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

/* Advances past tombstoned rows and yields the next live row index in
 * storage order. Returns false at end of table. */
bool cursor_next(Cursor *c, size_t *out_row);

#endif
