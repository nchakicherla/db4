#include "cursor.h"

void cursor_init(Cursor *c, const Table *t) {
    c->table    = t;
    c->next_row = 0;
}

bool cursor_next(Cursor *c, RowRef *out_row) {
    while (c->next_row < c->table->n_rows) {
        size_t row = c->next_row++;
        if (table_row_is_dead(c->table, row_ref(row))) continue;
        *out_row = row_ref(row);
        return true;
    }
    return false;
}
