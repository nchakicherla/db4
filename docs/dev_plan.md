# db4 development plan

## Vision

db4 is an embeddable C library, in the shape of sqlite3: link it in, call
`db4_open`/`db4_prepare`/`db4_step`/`db4_finalize`/`db4_close`, get a real SQL
engine with no server process. `bin/main` is not the product — it's a REPL
that happens to be the first consumer of the library's public API, same as
sqlite3's own `sqlite3` CLI is just a client of `libsqlite3`.

The engine is centered on CSV first: CSV is both the seed import format and,
initially, the on-disk representation. That's a deliberate simplification for
early milestones, not a permanent architecture — CSV has no schema types, no
random-access page structure, and no safe in-place mutation, so it will be
outgrown once atomicity and concurrency requirements exceed what "rewrite the
whole file" can deliver. The plan below treats that transition as a planned
milestone (M5), not a rewrite emergency.

Non-goals for now: no client/server mode, no replication, no distributed
anything, no SQL surface beyond a useful subset (see M6). Depth over breadth —
a small SQL grammar executed correctly and atomically beats a large one
executed loosely.

## Where things stand

Already built, in `c/src` / `c/include`, carried over from the db3
prototype:

- [arena.h](../c/include/arena.h) / `arena.c` — bump allocator, never-free
  within a lifetime, sticky first-failure latch (`ArenaFailure`) instead of a
  status return at every call site. This is the memory discipline the rest of
  the engine should keep using: pages, rows, parsed ASTs, and query-local
  scratch space all want "throw it away as a unit" lifetimes, not
  malloc/free bookkeeping.
- [budget.h](../c/include/budget.h) / `budget.c` — process-wide RAM cap so a
  `.load` of a too-big file fails cleanly instead of thrashing or getting
  OOM-killed. Still relevant post-CSV: it should end up guarding the page
  cache's resident set the same way it guards the arena today.
- [csv.h](../c/include/csv.h) / `csv.c` — RFC 4180 row reader over an
  in-memory buffer.
- [field.h](../c/include/field.h) / `field.c` — per-cell type inference and
  validation (`FieldType`: INT/DOUBLE/BOOL/TEXT) over a field's raw string
  form. This is what `table.c` calls to type each CSV column as it's loaded,
  and later what a declared `CREATE TABLE` column type (M6) validates
  incoming values against.
- [table.h](../c/include/table.h) / `table.c` and
  [index.h](../c/include/index.h) / `index.c` — the column-oriented
  in-memory `Table`, ported from db3's `library-conversion` branch (not the
  `main` branch's version — that one calls `exit(1)` on OOM, which a library
  can't do to its host; the `library-conversion` version already speaks the
  same `ArenaFailure`-latch discipline as arena.c/budget.c here). Fixed-width
  columns (INT/DOUBLE/BOOL) live inline in per-column arrays; TEXT stores a
  `StringRef` into a separate append-only heap. Includes tombstone deletes
  (`table_delete_row`/`table_compact`), one primary key per table backed by
  a hash index (`index.c`'s `RowIndex`), and foreign-key *declarations*
  (`table_set_foreign_key`) — more than a minimal row store strictly needs,
  brought in because it's proven, tested code rather than something to
  re-derive later.
- [catalog.h](../c/include/catalog.h) / `catalog.c` — **M2's catalog**: a
  `Catalog` is an array of named `Table`s (`.load`/`.tables` need more than
  one table addressable at once), name -> `Table*` lookup via
  `catalog_find`/`catalog_put`. This is `session.c` renamed and trimmed per
  the plan's own suggestion (module map, M2) rather than a new file: the
  `current`/`session_current`/`session_current_name` fields and accessors
  were dropped as dead code (nothing outside `session.c` itself ever called
  them). No connection-owned arenas or txn state yet — that's still ahead,
  whenever M5/M8 need it.
- [schema.h](../c/include/schema.h) / `schema.c` — parses `.load`'s optional
  JSON schema-override argument (`{"col":"type", "id":{"type":"int",
  "primary":true, "references":"other.col", ...}}`) into a `SchemaOverride`,
  using the vendored cJSON. Ported as-is from db3.
- [load.h](../c/include/load.h) / `load.c` — **M1's CSV-to-`Table` loader**,
  adapted from db3's `cmd_load`: parses `.load <name> "<path>"
  [{schema}|"<schema.json>"]`, infers each column's `FieldType` from the
  first data row (or takes a schema override), streams the rest through
  `csv_reader_next_row` into a scratch `Table`, and only installs it into
  the `Catalog` once every row has passed primary-key/foreign-key
  validation — a malformed or constraint-violating load is rejected
  atomically, the scratch table is simply discarded. Also has `dump_csv`
  (new code, not ported — db3 never had a CSV-dump command) and the
  `.tables`/`.schema` print helpers. What did *not* come over from
  `cmd_load`'s home in `commands/common.c`: `cascade_on_delete`/
  `cascade_on_update`/`check_column_constraints`, which only matter once
  `UPDATE`/`DELETE` exist (M6) — `load.c` only needs `check_row_constraints`,
  used once per freshly-loaded row.
- [cursor.h](../c/include/cursor.h) / `cursor.c` — **M4's row iterator**: a
  `Cursor` walks a `Table`'s rows in storage order, skipping tombstoned
  ones, so `interp.c` doesn't need to know about `table_row_is_dead`
  itself.
- [interp.h](../c/include/interp.h) / `interp.c` — **M4's executor**:
  `interp_exec_select` runs a `SelectStmt` straight off the AST (no
  planner, no bytecode) against a `Catalog`-held `Table`. Column types are
  static, so `WHERE`'s type-checking (comparison operand compatibility,
  `AND`/`OR`/`NOT` operand must be boolean-shaped) is one validation pass
  before any row is read, not a per-row runtime check. Row matching is
  three-valued (`true`/`false`/`unknown`), the same as real SQL, so `NULL`
  propagates correctly through `AND`/`OR`/`NOT` (`FALSE AND unknown` stays
  `FALSE`; `TRUE OR unknown` stays `TRUE`) instead of being collapsed to a
  simpler two-valued approximation - only `TRI_TRUE` rows make it through.
  `ORDER BY` sorts matching row indices with `qsort` (`NULL` sorts first
  ascending / last descending); `LIMIT` truncates after sorting, since a
  correct limit has to see the final order first.
- Lexer/AST/parser grammar (M5): `INSERT INTO <table> [(<cols>)] VALUES
  (<literals>)`, `UPDATE <table> SET <col> = <literal>, ... [WHERE
  <expr>]`, `DELETE FROM <table> [WHERE <expr>]`, and `BEGIN
  [TRANSACTION]`/`COMMIT`/`ROLLBACK`. `INSERT`/`UPDATE` values are
  restricted to literals (`parse_literal` in `parser.c`) rather than the
  general expression grammar - there's no arithmetic yet to make anything
  richer meaningful there. `ast.h` wraps all seven statement kinds in one
  `Stmt`, parsed by `parser_parse_statement`.
- [interp.c](../c/src/interp.c) grew `interp_exec` (M5): dispatches a
  `Stmt` to `interp_exec_select`/`_insert`/`_update`/`_delete`, or to
  `BEGIN`/`COMMIT`/`ROLLBACK` handling. `INSERT`/`UPDATE` run the same
  `check_row_constraints` load.c's loader runs per row (PK not-null/
  unique, FK target exists) before the mutation is allowed to stand, and
  `DELETE`/an `UPDATE` of the primary key additionally run
  `check_pk_not_referenced` first: no FK action (`CASCADE`/`SET_NULL`) is
  implemented yet, so the only safe default is to block anything that
  would orphan a row elsewhere that references this one, regardless of
  which action was declared - a known, deliberate gap (real cascade/
  set-null behavior is still to be written), not silent corruption.
  A mutating statement with no explicit `BEGIN` open runs as its own
  autocommit transaction (begin, execute, commit-or-rollback) so a bare
  `INSERT` is exactly as durable as one wrapped in `BEGIN...COMMIT`.
- [txn.h](../c/include/txn.h) / `txn.c` — **M5's transaction manager**: a
  `Txn` holds an undo log (`UndoEntry`), one entry per mutated row.
  Undoing an insert tombstones the row (`table_delete_row`); undoing a
  delete clears the tombstone (new `table_undelete_row` in `table.c`);
  undoing an update restores the cell's prior value/null-state captured
  before the overwrite (TEXT is captured as a `StringRef`, not a copied
  string, since `table.c`'s heap is append-only and the old bytes stay
  valid until a `table_compact_heap` that never runs mid-transaction).
  `txn_commit` hands the caller the distinct table names touched, for
  `interp.c` to durably `dump_csv` each one - `txn.c` itself only knows
  about `Table`, not `Catalog`, matching the layered architecture (the
  transaction manager sits below the catalog, so it can't depend on it).
- [catalog.h](../c/include/catalog.h)'s `NamedTable` gained a `path` field
  (M5) - `catalog_put` now takes the source CSV path so `COMMIT` knows
  where to durably write each mutated table back to.
- `main.c` — a linenoise REPL dispatching `.load`, `.tables`, `.schema
  <table>`, `.dump <table> "<path>"`, `.parse <sql>`, `.quit`/`.exit`, and
  a bare (non-dot) line: parsed (as of M5, any statement, not just
  `SELECT`) and run against the catalog and the REPL's one `Txn`.
  **M1 is done**: verified end to end with multi-table loads, a valid
  foreign key reference, a rejected foreign key reference (atomic
  failure, confirmed the referencing table never got installed), and a
  round-trip dump. **M3 is done**: `.parse` verified against precedence
  (`AND` binding tighter than `OR`), parenthesized/`NOT` subexpressions,
  `''`-escaped string literals, `ORDER BY ... DESC`/`LIMIT`, and four
  distinct malformed-input cases (each latching exactly one error at the
  right line). **M4 is done**: verified against projection (`*` and
  explicit column lists), `WHERE` filtering with `AND`/`OR`/`NOT`/parens,
  static type-checking (`WHERE name = 5` correctly rejected as
  text-vs-int), unknown table/column errors, `NULL`'s three-valued
  behavior (`WHERE age = age` correctly excludes a `NULL`-age row instead
  of treating self-equality as always true), `ORDER BY ... DESC` with a
  `NULL` value, and `LIMIT`. **M5's short-term step is done**: verified
  autocommit `INSERT`/`UPDATE`/`DELETE` with PK/FK/arity/type-mismatch
  rejections, an explicit multi-statement/multi-table `BEGIN...COMMIT`
  (both tables' CSV files durably updated together), nested `BEGIN`
  correctly rejected, `COMMIT`/`ROLLBACK` with no active transaction
  correctly rejected, a `BEGIN...INSERT...ROLLBACK` that left both the
  in-memory table and the on-disk CSV file completely unchanged, `DELETE`/
  `UPDATE` of a referenced primary key correctly blocked, and no leftover
  `.tmp-*` files after any of the above. **M6 is done**: verified
  `CREATE TABLE` with PK/FK (including `ON DELETE`/`ON UPDATE` actions
  round-tripping through `.schema`) and its "already exists" rejection;
  multi-row `INSERT ... VALUES (...), (...)`; arithmetic in `WHERE`/`SET`/
  `VALUES` (`price * qty > 15`, `SET qty = qty * 2, price = -price`
  evaluating against pre-update state so assignments don't chain, computed
  `VALUES (2+3, 10-5)`); `qty / 0` producing `inf`/`nan` instead of
  crashing; `INNER JOIN` with qualified/unqualified column resolution, an
  ambiguous-column rejection when two joined tables share a column name,
  `SELECT *` with qualified per-table headers, and `ORDER BY` across a
  join; `COUNT(*)`/`COUNT`/`SUM`/`AVG` with `GROUP BY` (`NULL`s correctly
  excluded from `SUM`/`AVG`/`COUNT(col)`); the `GROUP BY`+`JOIN` and
  `GROUP BY`+`ORDER BY`/`LIMIT` rejections firing as clean errors rather
  than parser failures (including a `GROUP BY <table>.<col>` case that
  originally *did* surface as a raw parser error before a real parser gap
  - qualified names in `GROUP BY` weren't handled - got fixed); and a
  `CREATE TABLE` inside `BEGIN...ROLLBACK` correctly surviving the
  rollback (DDL isn't undo-logged) while a same-transaction `INSERT` in a
  pre-existing table was correctly undone.

M7 added the WAL (`wal.c`/`wal.h`) and a `flock()`-backed reader/writer
lock (`lock.c`/`lock.h`): commit now WAL-appends just the rows a
transaction touched instead of rewriting each table's whole CSV file, and
`.checkpoint <table>` folds the WAL back into the base CSV on demand. See
M7 below for the full write-up and its scope boundary (no live
"see-new-commits-without-reloading" story yet - that needs M8's
connection object first).

M8's public-API half is that connection object: `db4.h`/`db4.c` freezes
`db4_open`/`db4_prepare`/`db4_step`/`db4_column_*`/`db4_finalize`/
`db4_changes`/`db4_errmsg`/`db4_exec`, and `main.c`'s bare-SQL path now
runs through it instead of calling `parser.c`/`interp.c` directly.
`interp.c`'s `SELECT` execution had to stop `fprintf`ing a text table
directly and start materializing a `ResultSet` (`result.h`/`result.c`,
built from `Value`s now shared via `value.h`/`value.c`) for `db4_step`/
`db4_column_*` to iterate - `interp_exec` itself lost its `FILE *out` and
is a pure engine call now. Page storage (replacing CSV+WAL with a
fixed-size-page file plus b-tree indexes) is the still-outstanding half;
see M8 below for why it's staying deferred.

### Stale scaffolding notes

`table_set_foreign_key`/`table_set_primary_key` are now actually enforced
during `.load` (via `load.c`'s `check_row_constraints`), but only there —
there's no `UPDATE`/`DELETE` yet (M6) to enforce them against, so
`cascade_on_delete`/`cascade_on_update` (db3's `commands/common.c`) were
deliberately not ported; M6 will need to write or re-port that when
`UPDATE`/`DELETE` exist.

`table_compact`/`table_compact_heap` (reclaiming tombstoned rows / orphaned
heap bytes) are still unreferenced outside `table.c` — there's still no
`DELETE`/`UPDATE` yet to produce anything for them to reclaim.

db3's `tests/test_table.c`, `test_index.c`, `test_commands_load.c` etc.
(part of its ~8,400-assertion suite) were never ported. db4 instead grew its
own black-box regression suite after M8 (`c/tests/`): `run_tests.py` drives
`bin/main`'s REPL end to end across a battery of M1-M8 scenarios (load/dump/
schema, FK/PK enforcement, RFC4180 CSV quoting, parser precedence and error
recovery, `SELECT` projection/`WHERE`/three-valued `NULL`/`ORDER BY`/`LIMIT`,
autocommit and explicit transactions with rollback, arithmetic/joins/
aggregates, WAL replay/checkpoint/torn-frame tolerance, and two real
concurrent-writer processes racing commits against the same CSV+WAL), and
`test_db4_api.c` calls `db4.h` directly (prepare/step/column/finalize) the
way a real embedder would, rather than only through `main.c`'s text
formatting. Both pass clean under ASan+UBSan. Unit-level coverage of
`table.c`/`index.c`/`arena.c` in isolation (db3-style) is still not
ported — the current suite is end-to-end through the REPL and the public
API, not per-module.

## Guiding principles (carry these forward from db3)

- **No premature abstraction.** Don't build a generic storage-engine
  interface before there's a second storage engine to abstract over. CSV
  gets a concrete table representation; the page store comes later as its
  own concrete thing, and *then* — if the seams actually diverge — gets
  factored.
- **Arena-shaped lifetimes.** Anything with a "this whole thing goes away
  together" lifetime (a parsed statement, a query's intermediate rows, a
  transaction's undo buffer) is an arena candidate, not a tangle of
  individual frees.
- **Fail latched, not scattered.** The `ArenaFailure` pattern — record the
  first failure, let the caller check once at an operation boundary — is the
  right shape for the parser and VM too: a syntax error deep in a
  sub-expression shouldn't need a status code threaded through every
  recursive call.
- **Budget everything resident.** If it's an engine invariant that db4 stays
  a good tenant on a low-spec machine, that has to extend to whatever
  replaces "the whole CSV in one arena" — a page cache with an eviction
  policy, not just a bigger arena.

## Layered architecture (bottom to top)

```
 ┌─────────────────────────────────────────────────────┐
 │  public API (db4.h)  — db4_open/prepare/step         │
 ├─────────────────────────────────────────────────────┤
 │  REPL (main.c)         — thin API client             │
 ├─────────────────────────────────────────────────────┤
 │  SQL front end   — lexer, parser, AST                │
 │  planner         — AST -> logical -> physical        │
 │  execution engine — bytecode VM + cursors            │
 ├─────────────────────────────────────────────────────┤
 │  catalog / schema — tables, columns, types           │
 ├─────────────────────────────────────────────────────┤
 │  transaction manager — txn boundaries, locks         │
 │  concurrency control  — RW-lock / MVCC               │
 ├─────────────────────────────────────────────────────┤
 │  storage engine  — table/row access,                 │
 │                     durability (WAL/journal)         │
 ├─────────────────────────────────────────────────────┤
 │  pager / CSV loader — pages or CSV buffers           │
 ├─────────────────────────────────────────────────────┤
 │  arena, budget, field (done), sds, cJSON, linenoise  │
 └─────────────────────────────────────────────────────┘
```

Each layer only calls the one below it. The REPL and the eventual public API
are peers — both just call into the execution engine — which is what keeps
the API honest: if the REPL can do it, so can a host application.

## Why these conventions (SQLite as reference point)

db4's problem shape — embedded, single-process, ACID, SQL — is exactly
sqlite3's, so it's the right prior art to borrow from rather than invent
around:

- **Atomicity/durability**: SQLite gives you a choice of rollback journal
  (copy original pages out before overwriting, atomically delete the journal
  on commit) or WAL (append new page versions to a side log, readers use the
  old pages until a checkpoint folds the WAL back into the main file).
  WAL is the better target for db4 because it lets readers run concurrently
  with a writer without blocking — the rollback journal's writer-excludes-
  everyone model is simpler but a worse fit once M7 wants real concurrency.
- **Concurrency**: SQLite's default is "serialized" — one mutex, one thread
  in the engine at a time — with WAL mode adding one-writer/many-readers on
  top. db4 should adopt the same target: one-writer/many-readers via WAL,
  rather than inventing MVCC from scratch. Full MVCC (Postgres-style,
  multiple in-flight writers) is a non-goal — it's a large jump in
  complexity (visibility rules, vacuum, xmin/xmax bookkeeping) that isn't
  justified until there's a concrete workload that needs concurrent writers.
- **Execution model**: SQLite compiles SQL to bytecode (VDBE) run by a small
  register-based VM over cursors. A tree-walking interpreter over the AST
  is a legitimate lighter-weight alternative and is the better starting
  point for db4 (M6) — bytecode compilation is an optimization to reach for
  later if interpretation overhead actually shows up, not a day-one
  requirement.
- **Storage**: SQLite's b-tree pager is the long-term shape to converge on
  (fixed-size pages, free-list, table b-trees keyed by rowid, index
  b-trees keyed by indexed columns) — but that's M5+, after CSV-backed
  tables prove out the layers above them.

## Milestones

Each milestone should be independently mergeable and independently useful —
no milestone should require the next one to already exist to be worth having.

### M1 — Table: an in-memory row store over CSV — **done**

See "Where things stand" above for the full breakdown of what's in the
tree (`table.c`/`index.c`, `catalog.c`, `schema.c`, `load.c`) and what was
deliberately left out (UPDATE/DELETE cascades, tests). `main.c`'s REPL
supports `.load <name> "<path>" [schema]`, `.tables`, `.schema <table>`,
and `.dump <table> "<path>"`.

### M2 — Catalog — **done**

A registry of open tables (name -> `Table*`), formalized from M1's `Session`
into [catalog.h](../c/include/catalog.h) / `catalog.c`'s `Catalog` (see
"Where things stand" above). Still a plain struct, not yet owned by a
connection handle — that ownership (plus arenas and, later, transaction
state) is still ahead of us, whenever M5/M8 actually need it; no
`db4*`-shaped object exists yet and none was added prematurely here.

### M3 — SQL front end: lexer + parser -> AST — **done**

[lexer.h](../c/include/lexer.h) / `lexer.c` — hand-written tokenizer
(keywords matched case-insensitively, identifiers, `INT`/`FLOAT`/`STRING`
literals with SQL-style `''`-escaped strings, `--` line comments, the
comparison/punctuation set the grammar below needs). Malformed input
becomes a `TOK_ERROR` token rather than the lexer latching its own failure
- the parser is the actual error boundary, so there's only one latch to
check, not two.

[ast.h](../c/include/ast.h) — node types for a `SelectStmt` (column list or
`*`, table name, optional `WHERE` expression tree, optional single-column
`ORDER BY` with `ASC`/`DESC`, optional `LIMIT`) and `Expr` (columns;
`INT`/`DOUBLE`/`STRING`/`BOOL`/`NULL` literals; `NOT`; binary `=` `!=` `<`
`<=` `>` `>=` `AND` `OR`). Everything in the tree is allocated into one
`Arena` passed in by the caller - the whole parse is a single "throw it
away as a unit" lifetime, same shape as `csv.c`'s row arena.

[parser.h](../c/include/parser.h) / `parser.c` — recursive-descent parser
producing that AST from:

```sql
SELECT <cols> FROM <table> [WHERE <expr>] [ORDER BY <col> [ASC|DESC]] [LIMIT <n>] [;]
```

`WHERE`'s expression grammar is `or_expr := and_expr (OR and_expr)*`,
`and_expr := not_expr (AND not_expr)*`, `not_expr := NOT not_expr |
comparison`, `comparison := primary (cmp_op primary)?`, `primary :=
column | literal | '(' expr ')'` - enough to express real filters
(`WHERE a >= 1 AND (b = 2 OR NOT c != 'x')`) without any arithmetic, since
nothing here needs it yet.

Errors latch exactly like `ArenaFailure`: `Parser` holds a sticky
`failed`/`err`/`err_line`, `parser_fail()` only records the *first*
failure, and every parse function checks `p->failed` on entry - one error
surfaces at the caller boundary (`parser_parse_select` returns `NULL`,
`parser_error()`/`parser_error_line()` report it) instead of a status
code threaded through every grammar production.

M3 has no executor to run against yet (M4), so `main.c` grew a `.parse
<sql>` REPL command that lexes, parses, and pretty-prints the AST (or the
latched error) - proof the grammar works standalone, without M4 needing to
exist first.

### M4 — Execution engine: tree-walking interpreter over cursors — **done**

See "Where things stand" above for [cursor.h](../c/include/cursor.h) /
`cursor.c` and [interp.h](../c/include/interp.h) / `interp.c`. A bare SQL
line at the REPL now runs a real `SELECT` against loaded CSV tables: `*`
or an explicit column projection, `WHERE` with `AND`/`OR`/`NOT`/parens and
three-valued `NULL` handling, `ORDER BY <col> [ASC|DESC]`, `LIMIT <n>`.
No query planner yet — one reasonable execution order per query shape
(filter, then sort, then limit) - and this is a good place to pause and
get comfortable before touching durability.

### M5 — Durability: WAL and atomic commit — **short-term step done**

This is where CSV-as-storage gets its first real replacement pressure.
Introduces `INSERT`/`UPDATE`/`DELETE` and, with them, the requirement that a
crash mid-write can't corrupt the table. Concretely:

1. **Short term (done): whole-table snapshot + atomic rename** (write a
   new temp file, `fsync`, `rename()` over the original) — trivially
   atomic, no partial-write states possible, and it's the correct *first*
   step because it establishes the transaction boundary (`BEGIN`/`COMMIT`/
   `ROLLBACK`) and the "nothing durable until commit" contract without
   needing a page format yet. See "Where things stand" above for
   [txn.h](../c/include/txn.h) / `txn.c` and the lexer/parser/`interp.c`
   grammar this needed. `dump_csv` (in `load.c`, used by both `.dump` and
   commit) now does the temp-write/`fsync`/`rename` itself, so `.dump`
   gets the same atomicity for free.
2. **Follow-on (done, as part of M7): an append-only WAL** of row-level
   changes, so commit is an `fsync` of a small append rather than a
   rewrite of the whole table, with a checkpoint step that periodically
   folds the WAL back into the base CSV. See M7 below for `wal.c`/`wal.h`
   and how `interp_exec_commit`/`load_csv` were rewired to use it - this
   is also the direct precursor to M7's reader/writer coordination (a WAL
   is what makes "readers never block the writer" meaningful).

The txn API (`db4_begin`/`db4_commit`/`db4_rollback`, not yet public - only
`main.c`'s REPL calls into it, same as every milestone before M8) is solid:
single-writer correctness including rollback is proven end to end below.

### M6 — Wider SQL surface — **done**

Grew M5's restricted (literal-values-only) `INSERT`/`UPDATE`/`DELETE` and
added what wasn't there before, in the order below - each still a small
addition to the AST/lexer/parser and `interp.c`, not a rearchitecture:

- **Arithmetic** (`+ - * /`, unary `-`): `lexer.c` gained `TOK_PLUS`/
  `TOK_MINUS`/`TOK_SLASH`; `ast.h` gained `EXPR_NEG` and the four
  arithmetic `BinaryOp`s; `parser.c` gained the usual precedence ladder
  (`unary` binds tighter than `term` (`*`,`/`) binds tighter than `arith`
  (`+`,`-`) binds tighter than comparisons). This is what makes computed
  `SET`/`WHERE`/`VALUES` expressions meaningful, so `INSERT`/`UPDATE` now
  take the *full* expression grammar (`parse_or`) instead of M5's
  literal-only `parse_literal` (removed - nothing calls it anymore).
  `interp.c`'s type-checker (`infer_expr_type`) and evaluator
  (`eval_value`) grew arithmetic cases; division always widens to
  `DOUBLE` specifically to avoid integer division-by-zero undefined
  behavior in C - `x/0.0` safely yields IEEE-754 `inf`/`nan` rather than
  crashing.
- **Qualified columns**: `lexer.c` gained `TOK_DOT`; `Expr`'s `EXPR_COLUMN`
  case is now `{table, name}` (`table` `NULL` if unqualified) instead of a
  bare string - needed once a query can touch more than one table (joins)
  and for disambiguating same-named columns.
- **Multi-row `INSERT ... VALUES (...), (...), ...`**: `InsertStmt` now
  holds an array of `ValueRow`s instead of one. Row constraint failures
  still abort the whole statement at the first bad row (matching
  `UPDATE`/`DELETE`'s existing behavior) rather than skipping just that
  row. `VALUES` expressions are still barred from referencing columns
  (`expr_has_column_ref` rejects them with a clear error) - a fresh
  `INSERT` row has nothing meaningful to reference yet.
- **`CREATE TABLE <name> (<col> <type> [PRIMARY KEY] [REFERENCES
  <table>(<col>) [ON DELETE|UPDATE CASCADE|RESTRICT|SET NULL]], ...)`**:
  builds a scratch `Table` and only installs it into the `Catalog` once
  every column/PK/FK check passes (same atomic-install pattern as
  `load.c`'s loader). It's DDL - takes effect immediately, isn't
  undo-logged (there's no "undo a CREATE TABLE" in `txn.c`, same as most
  real engines keeping DDL outside row-level transactions), and since it
  has no CSV path, a `CREATE TABLE`'d table is memory-only until an
  explicit `.dump` gives it one - `COMMIT` only durably writes tables
  that already have a path.
- **`INNER JOIN <table> ON <expr>`** (one or more, no aliasing yet):
  `interp.c`'s evaluator was generalized from "one `Table`+row" to a
  `RowCtx` (an array of `{alias, Table*, row}` sources) so `WHERE`/`ON`/
  projection can resolve a column against whichever joined table declares
  it (or error "ambiguous" if more than one does). This single
  generalization is what UPDATE/DELETE/plain-SELECT's WHERE evaluation
  also now goes through (as a trivial one-source `RowCtx`), not a
  parallel code path. `exec_select_plain` builds the joined row set with
  a plain nested-loop join (still "no query planner yet, one reasonable
  execution order," same principle M4 already established) - correct
  before it's fast.
- **Aggregates (`COUNT`, `SUM`, `AVG`) and `GROUP BY`**: recognized by
  one-token lookahead (`peek_next` - is an identifier immediately
  followed by `(`?) rather than making `count`/`sum`/`avg` reserved
  keywords, so they stay usable as ordinary column names elsewhere. Scoped
  to a single table for now - combined with `JOIN`, or with `ORDER BY`/
  `LIMIT`, is a clear rejection (`interp_exec_select`'s `grouped` check),
  not silently ignored or half-supported. A plain (non-aggregate) column
  in the select list must be one of the `GROUP BY` columns, same rule
  real SQL enforces.

### M7 — Concurrency: one writer, many readers — **done**

Builds the WAL that M5 deferred, then the reader/writer lock on top of it,
in one pass — the lock alone would have had nothing real to coordinate:

- **`wal.c`/`wal.h`**: an append-only log of row-level frames, replacing a
  whole-table rewrite per commit. Each frame is `{magic, row index, kind,
  payload_len, checksum, payload}`; `kind` is either a full row (every
  column's current value, `is_null` bit plus type-specific bytes) or a
  tombstone (row now dead, no payload). Frames record a row's *absolute*
  current state, not a delta, so replaying one twice is harmless — that's
  what makes a torn last frame (a crash mid-`fwrite`) safe to just stop at:
  `wal_replay` reads frames in order and bails the instant a header's magic
  doesn't match, a `fread` comes up short, or a checksum fails, keeping
  everything applied before that point. `interp_exec_commit` (`interp.c`)
  now walks the still-live undo log *before* clearing it (it already had
  `table_name`/`row` for exactly this) to build one distinct-row-list per
  distinct table touched since `BEGIN`, and WAL-appends just those rows —
  `dump_csv` no longer runs on every commit. `load_csv` (`load.c`) replays
  `<path>.wal` onto the freshly-loaded base table before handing it to the
  catalog, so a reload sees every committed change. `wal_checkpoint` folds
  the WAL into the base CSV via the existing `dump_csv` atomic rewrite,
  then removes the WAL file — exposed at the REPL as `.checkpoint <table>`,
  a manual step for now (no size- or time-based auto-checkpoint trigger
  yet, nothing needs one until a workload shows the WAL growing enough to
  matter).
- **`lock.c`/`lock.h`**: an advisory, file-backed reader/writer lock keyed
  by a table's path (`flock()` on `<path>.lock` — `LOCK_SH` for readers,
  `LOCK_EX` for writers), the actual cross-*process* coordination M7 asks
  for. `load_csv` holds a shared lock across the base-file read + WAL
  replay (concurrent readers stack freely); `commit_wal_for_table`
  (`interp.c`) and `.checkpoint` each hold an exclusive lock only around
  their own append or rewrite — bounded and brief, never across a whole
  session, matching "readers never block the writer and vice versa beyond
  a brief lock to register a reader's snapshot point."

**Deliberate scope boundary**: db4 has no connection/session object yet
(that's M8) — each REPL process loads a table once into its own memory and
keeps it resident for the session, so a concurrent writer's commit isn't
*visible* to another process's already-open table without that process
re-`.load`ing. What this milestone actually delivers is the storage-level
half of "one writer, many readers": commits and checkpoints are now safe
to interleave across processes sharing the same CSV+WAL files (no torn
reads, no lost WAL frames), which is the real prerequisite for a live
multi-reader story once M8's connection API exists to make "stay
subscribed to new commits" a meaningful thing to ask for. Building that
live-refresh mechanism now, ahead of having any connection object to hang
it off of, would be exactly the premature abstraction this plan's guiding
principles warn against.

### M8 — Public C API + real page storage — **public API done, page storage still ahead**

Split into the two pieces the heading always implied were separable, and
only the first is built: page storage's own text already frames it as
conditional ("once CSV's rewrite-on-commit cost... actually becomes the
bottleneck — not preemptively"), and M7's WAL just addressed exactly that
cost, so there's nothing concrete yet pushing on it.

- **`db4.h`/`db4.c` (done)**: a public header modeled on sqlite3's
  open/prepare/step/column/finalize shape - `db4_open`, `db4_close`,
  `db4_prepare`, `db4_step`, `db4_column_count`/`_name`/`_type`/`_int64`/
  `_double`/`_bool`/`_text`, `db4_finalize`, `db4_changes`, `db4_errmsg`,
  and `db4_exec` (a `sqlite3_exec`-alike convenience wrapper: prepare,
  step to completion, one text-formatted callback per row). `Db4` is a
  plain exposed struct (a `Catalog` + `Txn` + error/changes state), not an
  opaque handle - consistent with every other type in this codebase
  (`Table`/`Catalog`/`Txn` are all fully exposed too); it just saves the
  caller from wiring the two pieces together by hand. `main.c`'s bare-SQL
  path (`cmd_sql`) now goes through `db4_prepare`/`db4_step`/`db4_column_*`
  instead of calling `parser_parse_statement`/`interp_exec` directly - the
  library boundary is real for the one thing that actually needed proving
  (SQL execution); the pre-existing `.load`/`.dump`/`.schema`/`.tables`/
  `.checkpoint` dot-commands still work directly against `Db4`'s embedded
  `Catalog`, since those were never SQL statements sqlite has an
  equivalent surface for either.
  - This forced `interp.c`'s two `SELECT` execution paths
    (`exec_select_plain`/`exec_select_grouped`) off directly `fprintf`ing
    a text table and onto materializing a `ResultSet` (`result.h`/
    `result.c`: column names plus a flat, row-major `Value` array - see
    `value.h`/`value.c`, `Value`'s new shared home) that `db4_step`/
    `db4_column_*` iterate row-by-row. `interp_exec` lost its `FILE *out`
    entirely - it's a pure engine call now, taking an optional
    `ResultSet *out_rs` and `size_t *out_changes` instead of printing
    anything itself. Turning that back into human-readable text (the
    `col | col | col` / row / `(N rows)` format, and `BEGIN`/`COMMIT`/
    `N rows inserted`-style confirmations) is `main.c`'s job now, driven
    by `db4_column_*` and `db4_stmt_ast`'s exposed `StmtKind` - not the
    engine's.
  - `db4_prepare` parses exactly one statement per call (`out_tail`, if
    requested, points at the unconsumed remainder - always true EOF today,
    since this grammar already requires a whole statement to consume the
    entire input). Nothing chains multiple statements out of one buffer
    yet; the tail pointer exists because the signature needs it to mean
    something, not because a caller already relies on it.
- **Page storage (not started)**: replacing CSV+WAL with a fixed-size-page
  file format (page size, free list, a table b-tree keyed by rowid), with
  indexes (b-tree on a declared column) becoming viable once storage is
  page-based. Still gated on an actual bottleneck showing up - CSV import/
  export would become a feature (`.import`/`.dump`-equivalent) rather than
  the storage engine itself whenever this does get built.

## Module map (new files this plan implies)

| File | Purpose | Milestone |
|---|---|---|
| `table.c`/`table.h`, `index.c`/`index.h` | column/row store + PK hash index (done — ported from db3) | M1 |
| `schema.c`/`schema.h`, `load.c`/`load.h` | JSON schema overrides, CSV loader/dumper (done — ported/adapted from db3) | M1 |
| `catalog.c`/`catalog.h` | name -> `Table*` registry (done — renamed/trimmed from `session.c`); connection object still to come | M2 |
| `lexer.c`/`lexer.h` | SQL tokenizer (done) | M3 |
| `parser.c`/`parser.h` | recursive-descent parser -> AST (done) | M3 |
| `ast.h` | AST node types (done) | M3 |
| `cursor.c`/`cursor.h` | row iteration over a `Table` (done) | M4 |
| `interp.c`/`interp.h` | tree-walking query executor (done) | M4 |
| `txn.c`/`txn.h` | BEGIN/COMMIT/ROLLBACK, undo/redo bookkeeping (done) | M5 |
| `wal.c`/`wal.h` | append-only write-ahead log, checkpointing (done) | M5 (follow-on) / M7 |
| `lock.c`/`lock.h` | reader/writer coordination (done) | M7 |
| `value.h`/`value.c` | shared typed-`Value` currency (moved out of `interp.c`, done) | M8 |
| `result.h`/`result.c` | `ResultSet` a `SELECT` materializes into (done) | M8 |
| `db4.h`/`db4.c` | public API surface (done) | M8 |
| `pager.c`/`pager.h` | fixed-size page file, free list | M8 (not started) |
| `btree.c`/`btree.h` | table/index b-trees over pages | M8 (not started) |

## Open questions to resolve as we go (not blocking, but worth flagging)

- **Rowid model**: implicit integer rowid (sqlite's default) vs. requiring a
  declared primary key. Implicit rowid is simpler and matches the CSV
  import story (CSV rows have no natural key) — lean that way unless a
  concrete need for declared PKs shows up.
- **Type system strictness**: SQLite is famously dynamically typed
  ("type affinity", not enforcement). Given CSV's untyped origins, db4
  probably wants the same leniency, at least through M6.
- **Single translation unit vs. amalgamation**: sqlite3 ships as one giant
  `sqlite3.c`. Not a decision to make now — keep files separate through at
  least M6, revisit only if distribution/build-simplicity actually asks
  for it.
