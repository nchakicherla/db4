#ifndef TXN_H
#define TXN_H

#include <stdbool.h>
#include <stddef.h>

#include "table.h"

typedef enum {
    UNDO_INSERT,
    UNDO_UPDATE,
    UNDO_DELETE,
} UndoKind;

/* table_name is a borrowed pointer into the Catalog's NamedTable.name -
 * stable for the life of the session, since nothing renames or drops
 * tables yet. txn.c itself only knows about Table, not Catalog (the
 * layered architecture puts the transaction manager below the catalog),
 * so table_name travels alongside the Table* purely so a higher layer
 * (interp.c) can find which tables to durably flush at commit without
 * re-deriving it. */
typedef struct {
    UndoKind    kind;
    Table      *table;
    const char *table_name;
    size_t      row;

    /* UNDO_UPDATE only - the cell's value from just before it was
     * overwritten, restored verbatim on rollback. TEXT is captured as a
     * StringRef rather than a copied string: table.c's heap is
     * append-only, so an old StringRef stays valid until table_compact_heap
     * runs, which never happens mid-transaction. */
    size_t    col;
    FieldType type;
    bool      was_null;
    union {
        int64_t   i;
        double    d;
        bool      b;
        StringRef s;
    } old;
} UndoEntry;

typedef struct {
    bool       active;
    UndoEntry *log;
    size_t     count;
    size_t     cap;
} Txn;

void txn_init(Txn *txn);
void txn_term(Txn *txn);

bool txn_begin(Txn *txn, char *err, size_t err_len);

bool txn_log_insert(Txn *txn, Table *table, const char *table_name, size_t row);
bool txn_log_update(Txn *txn, Table *table, const char *table_name, size_t row, size_t col);
bool txn_log_delete(Txn *txn, Table *table, const char *table_name, size_t row);

/* Undoes every logged change, most recently logged first, then clears the
 * log and deactivates the txn. */
void txn_rollback(Txn *txn);

#endif
