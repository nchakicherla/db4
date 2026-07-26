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
- [session.h](../c/include/session.h) / `session.c` — a `Session`: an
  array of named `Table`s (`.load`/`.tables` need more than one table
  addressable at once). Ported as-is from db3; this is the same job M2's
  `catalog.c` is planned to formalize into a connection-scoped registry —
  for now `Session` *is* the catalog, informally.
- [schema.h](../c/include/schema.h) / `schema.c` — parses `.load`'s optional
  JSON schema-override argument (`{"col":"type", "id":{"type":"int",
  "primary":true, "references":"other.col", ...}}`) into a `SchemaOverride`,
  using the vendored cJSON. Ported as-is from db3.
- [load.h](../c/include/load.h) / `load.c` — **M1's CSV-to-`Table` loader**,
  adapted from db3's `cmd_load`: parses `.load <name> "<path>"
  [{schema}|"<schema.json>"]`, infers each column's `FieldType` from the
  first data row (or takes a schema override), streams the rest through
  `csv_reader_next_row` into a scratch `Table`, and only installs it into
  the `Session` once every row has passed primary-key/foreign-key
  validation — a malformed or constraint-violating load is rejected
  atomically, the scratch table is simply discarded. Also has `dump_csv`
  (new code, not ported — db3 never had a CSV-dump command) and the
  `.tables`/`.schema` print helpers. What did *not* come over from
  `cmd_load`'s home in `commands/common.c`: `cascade_on_delete`/
  `cascade_on_update`/`check_column_constraints`, which only matter once
  `UPDATE`/`DELETE` exist (M6) — `load.c` only needs `check_row_constraints`,
  used once per freshly-loaded row.
- `main.c` — a linenoise REPL dispatching `.load`, `.tables`, `.schema
  <table>`, `.dump <table> "<path>"`, `.quit`/`.exit`. **M1 is done**:
  verified end to end with multi-table loads, a valid foreign key
  reference, a rejected foreign key reference (atomic failure, confirmed
  the referencing table never got installed), and a round-trip dump.

Nothing here does SQL, transactions, or concurrency yet — that's the entire
subject of the rest of this plan.

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

No test suite was ported. db3 has `tests/test_table.c`, `test_index.c`,
`test_commands_load.c` etc. (part of its ~8,400-assertion suite) covering
exactly this code; db4 has no test infrastructure yet at all, consistent
with where this project already stood ("no tests yet").

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
tree (`table.c`/`index.c`, `session.c`, `schema.c`, `load.c`) and what was
deliberately left out (UPDATE/DELETE cascades, tests). `main.c`'s REPL
supports `.load <name> "<path>" [schema]`, `.tables`, `.schema <table>`,
and `.dump <table> "<path>"`.

### M2 — Catalog

A registry of open tables (name -> `Table*`) scoped to a `db4` connection
handle. This is also where the public API's connection object (`db4*`)
starts to take shape, even before M8 makes it public: something owns the
catalog, the arenas, and (later) the transaction state.

### M3 — SQL front end: lexer + parser -> AST

Hand-written lexer (tokens: keywords, identifiers, literals, operators,
punctuation) and a recursive-descent parser, producing an AST. Start with
just enough grammar to express M1/M2's capabilities as SQL:

```sql
SELECT <cols> FROM <table> [WHERE <expr>] [ORDER BY <col>] [LIMIT <n>];
```

Parse errors should latch like `ArenaFailure` does: one error recorded at
first failure, surfaced at the caller boundary, not threaded through every
grammar production.

### M4 — Execution engine: tree-walking interpreter over cursors

A `Cursor` abstraction iterates a `Table`'s rows; a small interpreter walks
the AST directly against cursors (filter for WHERE, project for column list,
in-memory sort for ORDER BY). No query planner yet — one reasonable
execution order per query shape. This is enough to run real `SELECT`
queries against loaded CSV tables and is a good place to pause and get
comfortable before touching durability.

### M5 — Durability: WAL and atomic commit

This is where CSV-as-storage gets its first real replacement pressure.
Introduce `INSERT`/`UPDATE`/`DELETE` and, with them, the requirement that a
crash mid-write can't corrupt the table. Concretely:

1. Short term: whole-table snapshot + atomic rename (write a new temp file,
   `fsync`, `rename()` over the original) — trivially atomic, no
   partial-write states possible, and it's the correct *first* step because
   it establishes the transaction boundary (`BEGIN`/`COMMIT`/`ROLLBACK`) and
   the "nothing durable until commit" contract without needing a page format
   yet.
2. Follow-on: an append-only WAL of row-level changes (`txn_id`, op, row
   bytes, checksum) so commit is an `fsync` of a small append rather than a
   rewrite of the whole table; a checkpoint step periodically folds the WAL
   back into the base CSV. This is the direct precursor to M7 (WAL is what
   also buys concurrent readers).

Get the txn API (`db4_begin`/`db4_commit`/`db4_rollback`) solid here, before
concurrency, so single-writer correctness (including crash recovery) is
proven first.

### M6 — Wider SQL surface

`INSERT INTO ... VALUES`, `UPDATE ... SET ... WHERE`, `DELETE FROM ... WHERE`,
`CREATE TABLE` (so a table can be declared instead of only inferred from a
CSV load), basic joins (`INNER JOIN`), aggregates (`COUNT`, `SUM`, `AVG`,
`GROUP BY`). Grow the grammar in `table.c`/M4's interpreter increment by
increment — each new clause should be a small addition to the AST and the
interpreter, not a rearchitecture.

### M7 — Concurrency: one writer, many readers

Reader-writer lock at the connection/catalog level, backed by the M5 WAL:
readers see a consistent snapshot (the WAL frames as of when their read
started, or the base file if the WAL is empty), a single writer appends new
WAL frames, readers never block the writer and vice versa beyond a brief
lock to register a reader's snapshot point. This is the "convention" for
embedded ACID engines under concurrent access (see WAL discussion above) —
resist the pull toward full MVCC until a concrete workload demands
concurrent *writers*, not just concurrent readers.

### M8 — Public C API + real page storage

- Freeze a `db4.h` public header modeled on sqlite3's shape:
  `db4_open`, `db4_close`, `db4_prepare`, `db4_step`, `db4_column_*`,
  `db4_finalize`, `db4_exec` (convenience wrapper), `db4_errmsg`.
  `main.c`'s REPL gets rewritten as a client of this header, proving the
  library boundary is real rather than main.c reaching into internals.
- Replace the CSV-file-as-storage model with a fixed-size-page file format
  (page size, free list, a table b-tree keyed by rowid) once CSV's
  rewrite-on-commit cost or lack of indexing actually becomes the
  bottleneck — not preemptively. CSV import/export becomes a feature
  (`.import`/`.dump`-equivalent) rather than the storage engine itself.
- Indexes (b-tree on a declared column) become viable once storage is
  page-based, enabling `WHERE`/join clauses to avoid full scans.

## Module map (new files this plan implies)

| File | Purpose | Milestone |
|---|---|---|
| `table.c`/`table.h`, `index.c`/`index.h` | column/row store + PK hash index (done — ported from db3) | M1 |
| `session.c`/`session.h`, `schema.c`/`schema.h`, `load.c`/`load.h` | named-table registry, JSON schema overrides, CSV loader/dumper (done — ported/adapted from db3) | M1 |
| `catalog.c`/`catalog.h` | name -> `Table*` registry, connection state — may just be `session.c` renamed/extended rather than a new file | M2 |
| `lexer.c`/`lexer.h` | SQL tokenizer | M3 |
| `parser.c`/`parser.h` | recursive-descent parser -> AST | M3 |
| `ast.h` | AST node types | M3 |
| `cursor.c`/`cursor.h` | row iteration over a `Table` | M4 |
| `interp.c`/`interp.h` | tree-walking query executor | M4 |
| `txn.c`/`txn.h` | BEGIN/COMMIT/ROLLBACK, undo/redo bookkeeping | M5 |
| `wal.c`/`wal.h` | append-only write-ahead log, checkpointing | M5 |
| `lock.c`/`lock.h` | reader/writer coordination | M7 |
| `pager.c`/`pager.h` | fixed-size page file, free list | M8 |
| `btree.c`/`btree.h` | table/index b-trees over pages | M8 |
| `db4.h` | public API surface | M8 |

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
