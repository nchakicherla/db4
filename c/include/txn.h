#ifndef TXN_H
#define TXN_H

#include <stdbool.h>
#include <stddef.h>

#include "catalog.h"
#include "table.h"

typedef enum {
    UNDO_INSERT,
    UNDO_UPDATE,
    UNDO_DELETE,
} UndoKind;

/* table_idx indexes into the Catalog this Txn is running against, resolved
 * to a live Table* only at undo/commit time (txn_rollback and interp.c's
 * commit path both take a Catalog* for exactly this). A raw Table* used to
 * be stored directly here, but catalog_put's realloc of catalog->tables[]
 * (e.g. a .load or CREATE TABLE later in the same transaction) can move
 * that array out from under an already-logged entry - storing the index
 * instead means growth never invalidates it. table_name is still a
 * borrowed pointer into the Catalog's NamedTable.name (stable for the life
 * of the session), kept alongside purely so interp.c's commit path can
 * find which tables to durably flush without re-deriving it. */
typedef struct {
    UndoKind    kind;
    size_t      table_idx;
    const char *table_name;
    RowRef      row;

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

bool txn_log_insert(Txn *txn, size_t table_idx, const char *table_name, RowRef row);
/* table is the same table table_idx resolves to right now - passed
 * separately (not re-derived from catalog here) because txn_log_update
 * needs to read the cell's current value immediately, before it's
 * overwritten, and the caller already has the pointer in hand. */
bool txn_log_update(Txn *txn, Table *table, size_t table_idx, const char *table_name, RowRef row, size_t col);
bool txn_log_delete(Txn *txn, size_t table_idx, const char *table_name, RowRef row);

/* Undoes every logged change, most recently logged first, then clears the
 * log and deactivates the txn. catalog resolves each entry's table_idx to
 * its current Table* - see the UndoEntry comment above for why that's
 * looked up now rather than stored. */
void txn_rollback(Txn *txn, Catalog *catalog);

/* Undoes only the entries logged since mark (an earlier txn->count, taken
 * by interp_exec_insert/update/delete before that statement's row loop
 * starts), most recently logged first, then truncates the log back to
 * mark - a single statement's own savepoint, not the whole transaction's:
 * txn->active and every earlier entry are left untouched, so a mid-
 * statement constraint failure (e.g. row 3 of a multi-row INSERT, or an
 * UPDATE that gets partway through matching rows before one fails a check)
 * can undo just its own partial work and report failure, while everything
 * an earlier statement in the same still-open transaction already did
 * stays exactly as it was - the same all-or-nothing guarantee autocommit
 * already got for free by wrapping a whole statement in begin/commit/
 * rollback, now true for a statement running inside an explicit BEGIN
 * alongside others too. */
void txn_rollback_to(Txn *txn, Catalog *catalog, size_t mark);

#endif
