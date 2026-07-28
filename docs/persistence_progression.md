# db4 persistence progression

The staged plan for replacing CSV+row-WAL with real page storage — the
"Page storage (not started)" half of M8 in [dev_plan.md](dev_plan.md),
expanded into independently mergeable stages.

This document is the working plan for that migration. `dev_plan.md`
remains the milestone history; this is the depth-first drill into the one
milestone that's large enough to need its own sequencing.

## Why now

Every performance win so far — the PK-indexed `WHERE` lookup, the
index-accelerated join, the row-level WAL replacing rewrite-on-commit —
has been built on top of one assumption: **the entire table is resident in
memory as contiguous per-column arrays, addressed by a dense row index.**
That assumption is now the binding constraint, and it's load-bearing in
more places every milestone:

- `Table` (`table.c`) allocates every column as one array sized to
  `row_cap`; a table larger than RAM is simply not representable.
- `Cursor` (`cursor.c`) walks `0..n_rows` and skips tombstones — row
  numbers *are* storage addresses.
- `RowIndex` (`index.c`) maps a hash to a row number, so the PK index is
  only meaningful while those numbers stay stable.
- `wal.c` frames are keyed by absolute row number, and `wal_replay` grows
  the table to fit them.
- `load.c` reads the whole base CSV into one arena before anything runs.

None of that is wrong for what it was built for. But the cost of unwinding
it grows with every feature layered on top, which is the actual argument
for doing it before widening the SQL surface further: secondary indexes,
a query planner, and larger-than-RAM scans all want to be built *against*
page storage, not ported onto it afterward.

## What this is not

Scope discipline, matching `dev_plan.md`'s non-goals:

- **Not MVCC.** The target stays one-writer/many-readers, as M7 already
  established. Page-level versioning for concurrent writers is a separate,
  much larger jump and nothing has asked for it.
- **Not a query planner.** The executor stays a tree-walking interpreter.
  Page storage makes a planner *worth* building later (real cost
  estimates need real page counts); it doesn't require one now.
- **Not a storage-engine abstraction layer.** Per the guiding principles:
  don't build the generic interface before there's a second engine to
  abstract over. The b-tree store replaces the array store; it doesn't get
  a vtable alongside it.
- **Not a CSV removal.** CSV survives as an import/export *feature*
  (`.import` / `.dump`), which is what it should have been all along. It
  stops being the on-disk representation.

## Design decisions (resolved up front)

These were flagged as open questions in `dev_plan.md`; the migration needs
them settled before Stage 1, so they're settled here.

- **Rowid model: implicit 64-bit integer rowid.** Follows sqlite, matches
  the CSV import story (imported rows have no natural key), and gives the
  table b-tree a monotonic key that appends cheaply. A declared `PRIMARY
  KEY` becomes a *secondary* index over the rowid tree (Stage 6), not the
  storage key. This is the single most consequential choice here: it means
  a row's identity survives page splits and compaction, which today's
  dense row numbers do not.
- **Page size: 4096 bytes, fixed at compile time to start.** Matches the
  common filesystem block size. A page-size field goes in the file header
  anyway, so making it configurable later is a parameter change rather
  than a format break.
- **Big-endian keys, little-endian everything else.** Rowid keys are
  compared as byte strings inside the b-tree, so they get big-endian
  encoding to make `memcmp` ordering match numeric ordering. Payload
  fields keep the native little-endian encoding `wal.c` already uses.
- **Cell format reuses `wal.c`'s row encoding.** `encode_row` already
  serializes a row as `is_null` byte + type-specific bytes per column,
  and `apply_row_payload` already decodes it. That's exactly the on-page
  cell payload format needed, already tested by every WAL round-trip in
  the suite. Extract it into `record.c` rather than writing a second
  serializer.
- **Type strictness unchanged.** Values keep their current
  inferred/declared `FieldType`; page storage changes where bytes live,
  not what they mean.

## Stages

Each stage is independently mergeable, leaves the suite green, and is
useful on its own. The ordering is deliberately "build the new thing
beside the old thing, then swap" — there is no point where the tree is
half-migrated and untestable.

### Stage 0 — Narrow the seam (no new storage yet) — **done**

**Goal:** make `Cursor` the *only* way the executor reaches a row, so
Stage 4's swap has exactly one contact surface instead of dozens.

Landed as a pure type-safety refactor: `RowRef` (`include/index.h`) is
now an opaque one-field struct, not a `size_t` alias - `row_ref()`/
`row_ref_raw()` are the only sanctioned way to cross into/out of it, and
every crossing outside `table.c`/`cursor.c`/`index.c` is a deliberate,
narrow seam (wire-format encoding in `wal.c`, `printf` diagnostics in
`load.c`), not a leak of the storage representation. `interp.c` - the
actual SQL executor - now has **zero** raw row-number touches: every
`table_*`/`read_column` call, every join/group-by/sort scratch array,
and the transaction undo log (`txn.h`'s `UndoEntry.row`) all carry
`RowRef` instead of `size_t`. The PK-index fast paths no longer reach
into `Table::pk_index` directly either - `interp.c` was doing that
before this stage, which was its own boundary violation; it now goes
through two new table.c-owned wrappers, `table_pk_index_usable`/
`table_find_by_pk_hash`, so nothing above the storage engine knows a
hash table is what's answering "does this row exist." `load.c`'s CSV
dumper was also switched from a raw `n_rows` loop to a `Cursor` walk,
closing a stray raw-row-reaching spot that predated this stage.

Verified behavior-preserving: clean rebuild with no new warnings, full
84-test suite passing on both the release build and an ASan/UBSan
build, and `test_db4_api.c` passing clean under ASan/UBSan.

Today `interp.c` reaches past the cursor and calls `table_get_int64`/
`table_get_text_ref`/`table_is_null` with raw row numbers, and probes
`pk_index` directly for the fast paths. That's fine against arrays and
fatal against a b-tree, where "row 7" isn't an address.

- Audit every `table_*` call in `interp.c` and `cursor.c`; the ones that
  take a row number are the migration surface.
- Introduce an opaque row handle (a `RowRef` — rowid plus whatever the
  store needs to locate it) and move column reads behind it.
- Re-express the PK fast path as "ask the index for a `RowRef`," not
  "ask the index for a row number."
- Keep everything backed by the existing arrays. **Nothing changes
  behaviorally in this stage** — it should be a pure refactor with all
  84 tests passing untouched.

**Done when:** no file outside `table.c`/`cursor.c` mentions a raw row
index. This is the stage that makes the rest safe, and it's the one most
worth resisting the urge to rush.

### Stage 1 — `pager.c` / `pager.h`: the page file

**Goal:** a fixed-size-page file with a cache, standalone and fully
testable before any b-tree exists.

- File header page (page 0): magic, format version, page size, page
  count, free-list head, schema cookie.
- `pager_get(pgno)` → pinned page buffer; `pager_unref`; `pager_mark_dirty`;
  `pager_flush`.
- Page cache with LRU eviction, **sized by `budget.c`** — this is the
  explicit "budget everything resident" principle applied to the thing
  that replaces "the whole CSV in one arena." A resident-set cap is the
  whole reason a larger-than-RAM table becomes possible.
- Free-list management: `pager_alloc_page` / `pager_free_page`.

**Testable standalone:** allocate pages, write patterns, evict under a
small budget, reopen the file, verify contents. No SQL involved.

### Stage 2 — `record.c` / `record.h`: cell encoding

**Goal:** row ↔ bytes, extracted from `wal.c` rather than reinvented.

- Move `encode_row`/`apply_row_payload`'s logic out of `wal.c` into
  `record.c`; `wal.c` becomes a caller.
- Add the inverse-direction helpers a b-tree needs that a WAL didn't:
  encoded size *before* encoding (to decide whether a cell fits a page),
  and decode-one-column-without-decoding-the-rest (projection doesn't
  need every column materialized).

**Done when:** `wal.c` has no serialization code of its own and the WAL
tests still pass — proving the extraction was behavior-preserving before
anything new depends on it.

### Stage 3 — `btree.c` / `btree.h`: the table b-tree

**Goal:** a rowid-keyed b-tree over `pager.c`, still with no connection to
`Table`.

- Leaf and interior page layouts: header, cell-pointer array, cells
  growing from the end, free space in the middle (sqlite's layout — it
  makes in-page insert/delete a pointer-array shuffle, not a memmove of
  every cell).
- Search, insert, delete, and in-order iteration by rowid.
- Page splitting on overflow; sibling merge or simple free-on-empty for
  deletes (a full rebalance is not required to be correct — under-full
  pages cost space, not accuracy).
- **Overflow pages** for cells larger than a page will hold. This is what
  finally kills the TEXT-heap-append-forever behavior — the same one whose
  compaction bug prompted this whole conversation.

**Testable standalone:** insert a few hundred thousand rowids in
sequential, reverse, and random order; verify in-order iteration, point
lookup, deletion, and that a reopened file reads back identically.

### Stage 4 — Swap `Table`'s backing store

**Goal:** `Table` keeps its API; its guts become a b-tree cursor.

This is the stage Stage 0 exists to make small. With the executor already
talking in `RowRef`s, `Table` can trade per-column arrays for a b-tree
without `interp.c` noticing.

- `table_*` accessors read through `pager`/`btree` instead of arrays.
- Tombstone deletes become real b-tree deletes; `table_compact` and
  `table_compact_heap` **disappear entirely** — a b-tree reclaims space
  structurally, so there's no heap to compact and no renumbering to do.
- `Cursor` becomes a b-tree cursor.
- `RowIndex`'s hash-to-row-number mapping becomes hash-to-rowid, which
  also ends the stale-index-after-`UPDATE` behavior documented as an
  accepted tradeoff in the PK-index commits — rowids are stable, so the
  entry doesn't go stale in the first place.

**Risk note:** this is the highest-risk stage. The full 84-test suite is
the safety net, and it should be run under ASan/UBSan at every step, not
just at the end.

### Stage 5 — WAL goes page-level

**Goal:** durability follows storage.

Row-level frames stop making sense once rows live in pages — a single
insert can split a page and touch three of them. The WAL becomes a log of
*page images*, which is both simpler and more general.

- Frame: `{page number, page image, checksum}` — and note the checksum
  now naturally covers the page number, since the fix in this repo's
  latest commit established exactly that discipline.
- Commit: append dirty page images, fsync, done.
- `wal_replay`: apply page images in order, stopping at the first torn
  frame — the same tolerate-a-torn-tail contract that already exists and
  is already tested.
- Checkpoint: fold WAL pages back into the main file, then truncate.
- Reader/writer coordination (`lock.c`) carries over unchanged in shape.

### Stage 6 — Secondary index b-trees

**Goal:** the payoff — indexes on columns other than the storage key.

Now that the table is a b-tree keyed by rowid, an index is just another
b-tree keyed by `(indexed column value, rowid)` whose payload is the
rowid. This is where `CREATE INDEX` becomes implementable, and where the
declared `PRIMARY KEY` stops being a special-cased hash table and becomes
a unique secondary index like any other.

The existing PK fast path in `interp.c` generalizes here from "is this a
PK equality?" to "is there an index covering this predicate?" — which is
also the first thing that genuinely resembles a query planner.

### Stage 7 — CSV becomes a feature

**Goal:** finish the demotion.

- `.import` reads a CSV into a page-backed table (today's `.load`,
  renamed to say what it actually does now).
- `.dump` stays as CSV export.
- `load.c` loses its role as the storage loader and becomes an importer.
- `db4_open` takes a database file path, which is the point at which db4
  stops being "a REPL over some CSVs" and becomes an embedded database
  with a file format.

## Sequencing rationale

The order is chosen so that **each stage is verifiable by something that
already exists**:

| Stage | Verified by |
|---|---|
| 0 — seam | the existing 84 tests, unchanged (pure refactor) |
| 1 — pager | new standalone pager tests, no SQL needed |
| 2 — record | the existing WAL tests (extraction must be behavior-preserving) |
| 3 — btree | new standalone b-tree tests, no SQL needed |
| 4 — swap | the existing 84 tests, unchanged (the real proof) |
| 5 — page WAL | the existing WAL/checkpoint/torn-frame/concurrency tests |
| 6 — indexes | the existing PK-index and join-index tests, plus new ones |
| 7 — CSV demotion | the existing load/dump tests, renamed |

Stages 1–3 build the new machinery with zero risk to a working engine.
Stage 4 is the one moment of real danger, and it arrives with the new
machinery already tested in isolation and the old test suite intact as the
oracle. That's the whole reason for this ordering rather than a
bottom-up rewrite.

## Testing additions this implies

The current suite is end-to-end through the REPL and the public API —
`dev_plan.md` already notes that per-module unit coverage was never
ported from db3. Stages 1 and 3 change that calculus: a pager and a
b-tree are exactly the kind of thing that *needs* unit tests, because
their failure modes (a bad split, a lost free-list entry, an eviction
race) don't reliably surface as a wrong SQL answer.

- `tests/test_pager.c` — allocation, eviction under a constrained
  budget, free-list reuse, reopen-and-verify.
- `tests/test_btree.c` — ordered/reverse/random insert, split
  correctness, delete, overflow-page round-trip, iteration.
- A crash-injection harness: kill the process mid-commit at varying
  offsets, reopen, assert the database is one of the two legal states
  (fully committed or fully not). The existing torn-frame test is a
  hand-rolled special case of this; page storage deserves the general
  version.

Everything continues to run under ASan+UBSan, and the existing 84 tests
stay the regression oracle throughout.

## Open questions (to resolve when the stage arrives)

- **Vacuum / free-page reuse policy.** Freed pages go on the free list;
  whether the file ever *shrinks* is a separate question. Defer to after
  Stage 4 — it's a space concern, not a correctness one.
- **Overflow-page threshold.** What fraction of a page a cell may occupy
  before spilling. sqlite uses a computed fraction; picking a simple
  constant first and tuning later is fine.
- **Schema persistence.** Today the schema lives in a JSON override
  argument or a `CREATE TABLE` statement, and a table's structure is
  re-derived on load. Page storage wants a real catalog table stored *in*
  the file. Probably Stage 4-adjacent, but it can start as a reserved
  header region.
- **Whether `budget.c` caps the page cache alone or total resident.**
  Once the arena is no longer holding whole tables, "the budget" mostly
  means "the page cache," which may make the existing accounting simpler
  rather than more complex.
