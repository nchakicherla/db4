#ifndef INTERP_H
#define INTERP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "ast.h"
#include "catalog.h"

/* Executes a parsed SELECT against catalog (filter for WHERE, project for
 * the column list, in-memory sort for ORDER BY, then LIMIT) and prints the
 * resulting rows to out. Returns false and fills err on the first problem
 * found (no such table/column, a WHERE/AND/OR/NOT operand or comparison
 * whose static types don't fit) - a plain first-error report, not
 * ArenaFailure-style latching: there's no recursive grammar production
 * here that latching protects, just one linear validation pass before any
 * row is touched. */
bool interp_exec_select(const SelectStmt *stmt, const Catalog *catalog, FILE *out, char *err, size_t err_len);

#endif
