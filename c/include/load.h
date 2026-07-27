#ifndef LOAD_H
#define LOAD_H

#include <stdbool.h>

#include "catalog.h"
#include "table.h"

#define MAX_COL_NAME_LEN 64

/* txn_active: true when a transaction is currently open on this Db4 - a
 * .load that would replace an existing table's Table struct (same name,
 * see catalog_put) is refused in that case, since any UndoEntry already
 * logged against that table's old row layout would otherwise no longer
 * mean anything once the replacement lands (see txn.h's UndoEntry comment).
 * Loading a brand-new name is unaffected - it can't invalidate anything
 * already logged. */
bool load_csv(Catalog *catalog, bool txn_active, const char *args);

bool dump_csv(const Table *t, const char *path);

void print_tables(const Catalog *catalog);
void print_schema(const Table *t);

#endif
