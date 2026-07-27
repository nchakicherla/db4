# db4

db4 is an embeddable, sqlite3-shaped SQL engine written in C. Link the
library in, call `db4_open`/`db4_prepare`/`db4_step`/`db4_finalize`/
`db4_close`, and you get a real SQL engine with no server process. The
`bin/main` REPL is just the first client of that public API - the same way
sqlite3's own CLI is a client of `libsqlite3`.

Storage today is CSV: on `.load` a CSV file becomes an in-memory table,
mutations are undo-logged and durably committed via a write-ahead log, and
`.dump`/checkpoints write back out to CSV. That's a deliberate first step,
not the final architecture - see [docs/dev_plan.md](docs/dev_plan.md) for
where CSV is headed (real page storage) and why that migration is
intentionally deferred.

## Scope

**Non-goals:** no client/server mode, no replication, no distributed
anything, no SQL surface beyond a useful subset. Depth over breadth - a
small SQL grammar executed correctly and atomically beats a large one
executed loosely.

## Capabilities

- **Storage**: CSV-backed tables, column-oriented in memory (fixed-width
  types inline, TEXT in an append-only heap), tombstone deletes, one
  primary key (hash-indexed) and foreign key declarations per table.
- **SQL surface**:
  - `SELECT` - `*` or explicit column projection, `WHERE` with
    `AND`/`OR`/`NOT`/parens and three-valued (`true`/`false`/unknown) NULL
    handling, `ORDER BY ... [ASC|DESC]`, `LIMIT`, `INNER JOIN` (one or
    more), `COUNT`/`SUM`/`AVG` with `GROUP BY`.
  - `INSERT INTO ... VALUES (...), (...), ...` (multi-row), `UPDATE ...
    SET ... WHERE ...`, `DELETE FROM ... WHERE ...`.
  - `CREATE TABLE` with `PRIMARY KEY` and `REFERENCES ... [ON DELETE|UPDATE
    CASCADE|RESTRICT|SET NULL]` (cascade/set-null actions are recorded;
    only the safe default - block anything that would orphan a
    reference - is currently enforced).
  - Arithmetic (`+ - * /`, unary `-`), table-qualified columns
    (`t.col`), bound parameters (`?` placeholders via `db4_bind_*`).
  - `BEGIN [TRANSACTION]` / `COMMIT` / `ROLLBACK`; a bare mutating
    statement with no open transaction runs as its own autocommit
    transaction.
- **Types**: `INT` / `DOUBLE` / `BOOL` / `TEXT`, inferred from a CSV
  column's first row or given via a schema override; SQL `NULL` throughout.
- **Durability**: an append-only write-ahead log of row-level changes
  (`.checkpoint <table>` folds it back into the base CSV), atomic
  temp-file/`fsync`/`rename` writes - a crash mid-write can't corrupt a
  table.
- **Concurrency**: an advisory, `flock()`-backed reader/writer lock per
  table file - one writer, many readers, safe across separate db4
  *processes* sharing the same CSV+WAL files.
- **Public C API**: `db4.h`, modeled on sqlite3's open/prepare/bind/step/
  column/reset/finalize shape (see below).

See [docs/architecture.md](docs/architecture.md) for the full layer-by-layer
breakdown and [docs/dev_plan.md](docs/dev_plan.md) for the milestone-by-
milestone history and what's intentionally not built yet (most notably:
page storage, still CSV+WAL for now).

## Building

```bash
cd c
make
```

Produces `bin/main`, a REPL over the public API. `make debug` builds an
unoptimized, symbol-ful binary; `make clean` removes build output.

## Using the REPL

```
$ bin/main
> .load customers "customers.csv"
> SELECT * FROM customers WHERE age >= 30 ORDER BY name
> BEGIN
> UPDATE customers SET age = age + 1 WHERE id = 1
> COMMIT
> .quit
```

Dot-commands (talk to the table catalog directly, no SQL equivalent):

| Command | Effect |
|---|---|
| `.load <name> "<path>" [schema]` | Load a CSV file as table `<name>`, optionally overriding inferred column types/keys with a JSON schema |
| `.tables` | List loaded tables |
| `.schema <table>` | Print a table's column types, primary key, foreign keys |
| `.dump <table> "<path>"` | Write a table back out to CSV |
| `.checkpoint <table>` | Fold the table's WAL back into its base CSV |
| `.parse <sql>` | Parse `<sql>` and print the AST, without executing it |
| `.quit` / `.exit` | Exit the REPL |

Any other input is parsed and executed as SQL through the public API
(`db4_prepare`/`db4_step`/`db4_column_*`), the same path any embedding
application uses.

## C API overview

`db4.h` is the public surface - `Db4` (a catalog + transaction state) and
`Db4Stmt` (a prepared statement) are both plain exposed structs, not opaque
handles, consistent with every other type in this codebase.

```c
#include "db4.h"

Db4 db;
db4_open(&db);

Db4Stmt *stmt;
const char *sql = "INSERT INTO users (id, name) VALUES (?, ?)";
db4_prepare(&db, sql, strlen(sql), &stmt, NULL);
db4_bind_int64(stmt, 1, 1);
db4_bind_text(stmt, 2, "ada", 3);
db4_step(stmt);          /* DB4_DONE - db4_changes(&db) == 1 */
db4_reset(stmt);          /* reuse the compiled statement */
db4_bind_int64(stmt, 1, 2);
db4_bind_text(stmt, 2, "grace", 5);
db4_step(stmt);
db4_finalize(stmt);

sql = "SELECT id, name FROM users ORDER BY id";
db4_prepare(&db, sql, strlen(sql), &stmt, NULL);
while (db4_step(stmt) == DB4_ROW) {
    size_t len;
    const char *name = db4_column_text(stmt, 1, &len); /* not NUL-terminated - use len */
    printf("%lld: %.*s\n", (long long)db4_column_int64(stmt, 0), (int)len, name);
}
db4_finalize(stmt);

db4_close(&db);
```

| Function | Purpose |
|---|---|
| `db4_open` / `db4_close` | Open/close a connection (a table catalog + transaction state) |
| `db4_prepare` | Parse one SQL statement into a `Db4Stmt` |
| `db4_stmt_ast` | Inspect the parsed AST behind a statement |
| `db4_bind_parameter_count` | Number of `?` placeholders in a prepared statement |
| `db4_bind_int64` / `_double` / `_bool` / `_text` / `_null` | Bind a value to a 1-based `?` placeholder |
| `db4_step` | Execute (first call) or advance to the next row (`SELECT`) |
| `db4_reset` | Return a statement to its pre-execution state for reuse, keeping its bindings |
| `db4_finalize` | Free a prepared statement |
| `db4_column_count` / `_name` / `_type` / `_int64` / `_double` / `_bool` / `_text` | Read the current row's columns |
| `db4_changes` | Rows affected by the last `INSERT`/`UPDATE`/`DELETE` |
| `db4_errmsg` | The last error message |
| `db4_exec` | Convenience: prepare, step to completion, one callback per row (`sqlite3_exec`-style) |

Parameters are never spliced into SQL text - `db4_bind_*` keeps a bound
value's type and content separate from the query's structure all the way
through execution, so a value can't be interpreted as part of the
statement it was passed into.

## Testing

```bash
cd c
make
python3 tests/run_tests.py       # black-box REPL regression suite
```

`tests/test_db4_api.c` exercises `db4.h` directly (build it against
`src/*.c`, excluding `main.c`, with `-fsanitize=address,undefined` to match
how it's normally run).

## License

MIT - see [LICENSE](LICENSE).
