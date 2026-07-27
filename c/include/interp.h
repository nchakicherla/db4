#ifndef INTERP_H
#define INTERP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "ast.h"
#include "catalog.h"
#include "txn.h"

/* Executes a parsed SELECT against catalog (filter for WHERE, project for
 * the column list, in-memory sort for ORDER BY, then LIMIT) and prints the
 * resulting rows to out. Returns false and fills err on the first problem
 * found (no such table/column, a WHERE/AND/OR/NOT operand or comparison
 * whose static types don't fit) - a plain first-error report, not
 * ArenaFailure-style latching: there's no recursive grammar production
 * here that latching protects, just one linear validation pass before any
 * row is touched. */
bool interp_exec_select(const SelectStmt *stmt, const Catalog *catalog, FILE *out, char *err, size_t err_len);

/* Executes any statement (SELECT/INSERT/UPDATE/DELETE/BEGIN/COMMIT/
 * ROLLBACK) against catalog, using txn for undo bookkeeping and
 * transaction state. A mutating statement (INSERT/UPDATE/DELETE) issued
 * with no transaction already open runs as its own autocommit
 * transaction: begin, execute, then commit on success or roll back on
 * failure, so a bare INSERT is just as durable/atomic as an explicit
 * BEGIN...COMMIT block around it. Returns false and fills err on failure. */
bool interp_exec(const Stmt *stmt, Catalog *catalog, Txn *txn, FILE *out, char *err, size_t err_len);

#endif
