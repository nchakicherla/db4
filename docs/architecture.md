# db4 architecture

db4 is an embeddable, sqlite3-shaped SQL engine written in C. Link the
library in, call `db4_open`/`db4_prepare`/`db4_step`/`db4_finalize`/
`db4_close`, and you get a real SQL engine with no server process. The
`bin/main` REPL is just the first client of that public API, the same way
sqlite3's own CLI is a client of `libsqlite3`.

Storage today is CSV: on `.load` a CSV file becomes an in-memory `Table`,
mutations are undo-logged and committed via a WAL, and `.dump`/checkpoints
write back out to CSV. That's a deliberate first step (see
[dev_plan.md](dev_plan.md)) — CSV is being outgrown in favor of real page
storage, but that migration hasn't started yet.

## Layers (bottom to top)

Each layer only calls the one below it.

```
 public API (db4.h)      db4_open/prepare/step/column/finalize
 REPL (main.c)           thin client of the public API
 ─────────────────────────────────────────────────────────────
 SQL front end           lexer -> parser -> AST
 execution engine        tree-walking interpreter over cursors
 ─────────────────────────────────────────────────────────────
 catalog / schema        named tables, schema overrides
 transaction manager     BEGIN/COMMIT/ROLLBACK, undo log
 concurrency control     flock-based reader/writer lock
 ─────────────────────────────────────────────────────────────
 storage engine          Table (column-oriented in-memory store)
                         durability: WAL + CSV
 ─────────────────────────────────────────────────────────────
 foundations             arena, budget, field, sds, cJSON, linenoise
```

## Module-by-module

### Foundations

- **[arena.h](../c/include/arena.h) / arena.c** — bump allocator: allocate
  freely, free the whole arena at once when its owning lifetime ends
  (a parsed statement, a loaded table, a query's scratch space). Tracks
  only a "first failure" flag (`ArenaFailure`) instead of a status code at
  every call site — one check at the end of an operation instead of
  threading error handling through every allocation.
- **[budget.h](../c/include/budget.h) / budget.c** — a process-wide RAM
  cap so loading a too-large CSV fails cleanly instead of thrashing or
  getting OOM-killed.
- **[field.h](../c/include/field.h) / field.c** — per-cell type inference
  (`FieldType`: INT/DOUBLE/BOOL/TEXT) and validation from a raw string.
  Used both to infer a CSV column's type on `.load` and to validate values
  against a declared `CREATE TABLE` column type.
- **external/** — vendored third-party code: `sds` (dynamic strings),
  `cJSON` (schema-override JSON parsing), `linenoise` (REPL line editing).

### Storage engine

- **[table.h](../c/include/table.h) / table.c** — the core in-memory row
  store, column-oriented: fixed-width types (INT/DOUBLE/BOOL) live inline
  in per-column arrays, TEXT is a `StringRef` into an append-only heap.
  Deletes are tombstones (`table_delete_row`/`table_undelete_row`, later
  reclaimed by `table_compact`/`table_compact_heap`). Also owns one
  primary-key hash index and foreign-key declarations per table.
- **[index.h](../c/include/index.h) / index.c** — `RowIndex`, the hash
  index backing a table's primary key lookup.
- **[csv.h](../c/include/csv.h) / csv.c** — an RFC 4180 row reader over an
  in-memory buffer.
- **[schema.h](../c/include/schema.h) / schema.c** — parses `.load`'s
  optional JSON schema-override argument into a `SchemaOverride` (column
  types, primary key, foreign-key references), via the vendored cJSON.
- **[load.h](../c/include/load.h) / load.c** — the CSV-to-`Table` loader:
  infers or applies a schema, streams rows in, validates every row's
  primary-key/foreign-key constraints, and only installs the result into
  the `Catalog` if the whole load succeeds (atomic: a bad load never
  partially registers a table). Also owns `dump_csv` (atomic temp-file /
  `fsync` / `rename` write-back) and the `.tables`/`.schema` print helpers.

### Durability & concurrency

- **[wal.h](../c/include/wal.h) / wal.c** — an append-only write-ahead
  log of row-level frames (full row state or a tombstone). Commit appends
  just the rows a transaction touched instead of rewriting the whole
  table's CSV. Frames encode absolute state, not deltas, so replaying one
  twice is harmless — which is what makes a torn last frame (crash
  mid-write) safe to just stop at. `wal_checkpoint` folds the WAL back
  into the base CSV on demand (`.checkpoint <table>` in the REPL).
- **[lock.h](../c/include/lock.h) / lock.c** — an advisory,
  `flock()`-backed reader/writer lock keyed by a table's file path.
  Readers (`.load`) take a shared lock across the read + WAL replay;
  writers (commit, checkpoint) take a brief exclusive lock only around
  their own append/rewrite. This is what lets separate db4 *processes*
  safely share the same CSV+WAL files.
- **[txn.h](../c/include/txn.h) / txn.c** — the transaction manager: a
  `Txn` holds an undo log, one `UndoEntry` per mutated row (enough to
  reverse an insert, update, or delete). `txn_rollback` undoes everything
  logged, most-recent first. A bare mutating statement with no open
  `BEGIN` still runs inside its own autocommit transaction, so a lone
  `INSERT` is exactly as durable as one wrapped in `BEGIN...COMMIT`.

### Catalog / schema

- **[catalog.h](../c/include/catalog.h) / catalog.c** — a `Catalog` is
  just an array of named `Table`s plus each one's source CSV path (needed
  so `COMMIT` knows where to write back to). `catalog_find`/`catalog_put`
  are the name -> `Table*` registry.

### SQL front end

- **[lexer.h](../c/include/lexer.h) / lexer.c** — hand-written tokenizer:
  case-insensitive keywords, identifiers, INT/FLOAT/STRING literals with
  SQL-style `''`-escaped strings, `--` comments, and the operator/
  punctuation set the grammar needs. Malformed input becomes a
  `TOK_ERROR` token rather than the lexer latching its own failure — the
  parser is the single error boundary.
- **[ast.h](../c/include/ast.h)** — node types for every statement kind
  (`SELECT`/`INSERT`/`UPDATE`/`DELETE`/`CREATE TABLE`/`BEGIN`/`COMMIT`/
  `ROLLBACK`, wrapped in one `Stmt`) and `Expr` (columns — qualified or
  not — literals, arithmetic, comparisons, boolean logic). Everything is
  allocated into one `Arena` passed in by the caller.
- **[parser.h](../c/include/parser.h) / parser.c** — recursive-descent
  parser producing that AST. Errors latch the same way `ArenaFailure`
  does: the `Parser` holds a sticky first-failure, and every parse
  function checks it on entry, so one error surfaces at the top rather
  than a status code threaded through every grammar production.

### Execution engine

- **[cursor.h](../c/include/cursor.h) / cursor.c** — a `Cursor` walks a
  `Table`'s rows in storage order, skipping tombstoned ones, so the
  interpreter doesn't need to know about dead rows itself.
- **[interp.h](../c/include/interp.h) / interp.c** — the tree-walking
  executor, no planner or bytecode: `interp_exec` dispatches an AST
  `Stmt` to per-kind handlers.
  - `SELECT`: projection (`*` or explicit columns), `WHERE` with
    three-valued (`true`/`false`/unknown) `NULL`-aware `AND`/`OR`/`NOT`,
    `ORDER BY`/`LIMIT`, `INNER JOIN` (nested-loop, resolved through a
    generalized multi-table `RowCtx`), and `COUNT`/`SUM`/`AVG` with
    `GROUP BY`.
  - `INSERT`/`UPDATE`/`DELETE`: run the same row-constraint checks
    (`check_row_constraints`) as `.load`, plus a check that a delete or
    primary-key update doesn't orphan a row that references it elsewhere
    (no `CASCADE`/`SET_NULL` execution yet — only the safe default of
    blocking).
  - `CREATE TABLE`: builds and atomically installs a new table (DDL,
    not undo-logged).
  - `BEGIN`/`COMMIT`/`ROLLBACK`: drives `txn.c`, and on commit walks the
    undo log to find which tables were touched and WAL-appends just
    those rows.
  - A `SELECT`'s output is materialized into a `ResultSet`
    (`result.h`/`result.c`) instead of being printed directly — this is
    what lets `db4_step`/`db4_column_*` hand rows back one at a time.
- **[value.h](../c/include/value.h) / value.c** — `Value`, the shared
  typed-and-possibly-null currency between the expression evaluator, a
  `ResultSet`'s cells, and `db4.h`'s column accessors. TEXT values are
  always borrowed (into a table's heap, a statement's arena, or a string
  literal) — `Value` never owns memory.
- **[result.h](../c/include/result.h) / result.c** — `ResultSet`: column
  names plus a flat, row-major `Value` array. What a `SELECT` builds and
  what `db4_step` iterates over.

### Public API & REPL

- **[db4.h](../c/include/db4.h) / db4.c** — the public C API, modeled on
  sqlite3's shape: `db4_open`/`db4_close`, `db4_prepare` (parses one
  statement), `db4_step` (executes, then advances row by row for a
  `SELECT`), `db4_column_count`/`_name`/`_type`/`_int64`/`_double`/
  `_bool`/`_text`, `db4_finalize`, `db4_changes`, `db4_errmsg`, and
  `db4_exec` (a `sqlite3_exec`-style convenience wrapper). `Db4` is a
  plain exposed struct (a `Catalog` + `Txn` + error/changes state), not
  an opaque handle — consistent with every other type in this codebase.
- **[main.c](../c/src/main.c)** — the linenoise-driven REPL. Dot-commands
  (`.load`, `.tables`, `.schema`, `.dump`, `.checkpoint`, `.parse`,
  `.quit`/`.exit`) talk to the `Catalog` directly, since they have no SQL
  equivalent. A bare line is SQL and goes through `db4_prepare`/
  `db4_step`/`db4_column_*` — `main.c` is a client of the public API, not
  a caller of `parser.c`/`interp.c`.

## Request flow

**Bare SQL line at the REPL** (`SELECT ... FROM ...`):

```
main.c: cmd_sql
  -> db4_prepare        (lexer.c -> parser.c -> AST, arena-allocated)
  -> db4_step           (interp.c: interp_exec dispatches on Stmt kind)
       SELECT -> interp_exec_select -> cursor.c walks live rows,
                 WHERE/JOIN/GROUP BY evaluated via value.c,
                 result.c materializes a ResultSet
       INSERT/UPDATE/DELETE -> row constraint checks, txn.c undo-logs
                 each change, autocommit if no BEGIN is open
  -> db4_column_*       (reads back out of the ResultSet / Db4Stmt)
  -> db4_finalize
```

**Commit**: `interp.c` walks the txn's undo log to find distinct
tables touched, takes an exclusive lock per table (`lock.c`), and
WAL-appends (`wal.c`) just those rows — no whole-table rewrite.

**`.load`**: `load.c` takes a shared lock, reads the base CSV, replays
`<path>.wal` on top of it (so a fresh load sees every committed change),
validates constraints, and installs the table into the `Catalog`.

## Testing

`c/tests/run_tests.py` drives `bin/main`'s REPL end to end (load/dump/
schema, constraint enforcement, parser edge cases, transactions, joins,
aggregates, WAL replay/checkpoint, and concurrent-writer races).
`c/tests/test_db4_api.c` exercises `db4.h` directly the way a real
embedder would, rather than only through the REPL's text output.

## Where this is headed

See [dev_plan.md](dev_plan.md) for the full milestone history and
reasoning. The next planned architectural change is replacing CSV+WAL
with real page storage (fixed-size pages, a table b-tree keyed by rowid,
and index b-trees) — deliberately deferred until CSV's rewrite-on-commit
cost is an actual bottleneck rather than a hypothetical one.

That migration now has its own staged plan in
[persistence_progression.md](persistence_progression.md), which is the
working document to develop against.
