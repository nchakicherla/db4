#ifndef DB4_H
#define DB4_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ast.h"
#include "catalog.h"
#include "txn.h"

/* db4's public C API - modeled on sqlite3's open/prepare/step/column/
 * finalize shape (M8). main.c's REPL is a client of this header for SQL
 * execution, not a caller of parser.c/interp.c directly - proving the
 * library boundary is real.
 *
 * A connection is, today, an in-memory table catalog plus transaction
 * state - there's no single-file database format yet (replacing CSV+WAL
 * with real page storage is a distinct, deliberately-deferred follow-on;
 * see dev_plan.md's M8 write-up). Db4 is a plain exposed struct, not an
 * opaque handle, matching every other type in this codebase (Catalog/
 * Table/Txn are all fully exposed too) - db4_open just saves the caller
 * from remembering to zero-initialize and pair its two fields. */
typedef struct {
    Catalog catalog;
    Txn     txn;
    char    errmsg[256];
    size_t  changes;
} Db4;

typedef struct Db4Stmt Db4Stmt;

enum { DB4_ROW, DB4_DONE, DB4_ERROR };
enum { DB4_INTEGER, DB4_FLOAT, DB4_BOOL, DB4_TEXT, DB4_NULL };

bool db4_open(Db4 *db);
void db4_close(Db4 *db);

/* Parses exactly one statement out of sql (sql need not be
 * NUL-terminated exactly at the statement's end). out_tail, if non-NULL,
 * receives a pointer just past what was consumed - sqlite3_prepare's
 * tail-pointer shape - though nothing in this codebase currently chains
 * multiple statements out of one buffer; it's here because it's what
 * this signature needs to mean, not because something already uses it.
 * Returns false and fills db4_errmsg on a parse error. */
bool db4_prepare(Db4 *db, const char *sql, size_t sql_len, Db4Stmt **out_stmt, const char **out_tail);

/* Borrows the parsed AST behind stmt (valid until db4_finalize) - lets a
 * caller that needs to know more than "did it return rows" (e.g. main.c
 * choosing what confirmation text to print for BEGIN vs CREATE TABLE vs
 * INSERT) inspect the statement directly, the same non-opaque-internals
 * style every other type in this codebase already uses (Table/Catalog/
 * Txn/Stmt are all fully exposed structs, not hidden behind accessors). */
const Stmt *db4_stmt_ast(const Db4Stmt *stmt);

/* Executes the statement (on the first call) or advances to the next row
 * (on later calls, SELECT only). DB4_ROW: a row is ready, read it via
 * db4_column_*. DB4_DONE: no more rows (or the statement never produced
 * any - a non-SELECT statement reports DB4_DONE the first time too, once
 * it has run). DB4_ERROR: it failed - see db4_errmsg. */
int db4_step(Db4Stmt *stmt);

void db4_finalize(Db4Stmt *stmt);

/* Column count/name are available as soon as this returns - unlike
 * db4_column_type/_int64/_double/_bool/_text below, which need a row
 * fetched by db4_step first. A SELECT's ResultSet (names included) isn't
 * actually built until the statement runs, so calling either of these
 * before any db4_step call runs the statement on the caller's behalf as a
 * side effect - matching sqlite3's contract (column metadata is known at
 * prepare time there) without requiring a caller to step first just to
 * learn the shape of what's coming. Non-const: this is why. */
int         db4_column_count(Db4Stmt *stmt);
const char *db4_column_name(Db4Stmt *stmt, int col);

/* DB4_NULL if the column's current-row value is NULL, else whichever of
 * DB4_INTEGER/DB4_FLOAT/DB4_BOOL/DB4_TEXT it actually holds. */
int db4_column_type(const Db4Stmt *stmt, int col);

int64_t     db4_column_int64(const Db4Stmt *stmt, int col);
double      db4_column_double(const Db4Stmt *stmt, int col);
bool        db4_column_bool(const Db4Stmt *stmt, int col);
const char *db4_column_text(const Db4Stmt *stmt, int col, size_t *out_len);

/* Rows affected by the most recently completed INSERT/UPDATE/DELETE (0
 * after every other statement kind) - sqlite3_changes()'s equivalent. */
size_t db4_changes(const Db4 *db);

const char *db4_errmsg(const Db4 *db);

/* Called once per result row - every column already formatted as text
 * (NULL for a SQL NULL), sqlite3_exec's callback shape. Returning false
 * stops iteration early without that being an error. */
typedef bool (*Db4ExecCallback)(void *ctx, int n_cols, const char **col_text, const char **col_names);

/* Convenience wrapper: prepares sql, steps it to completion, and - for a
 * SELECT - invokes cb once per row. Returns false and fills db4_errmsg on
 * failure (a parse error, an execution error, or db4_prepare leaving
 * trailing input - db4_exec runs exactly one statement, like the rest of
 * this API). */
bool db4_exec(Db4 *db, const char *sql, Db4ExecCallback cb, void *ctx);

#endif
