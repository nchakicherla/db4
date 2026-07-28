#ifndef WAL_H
#define WAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "table.h"

/* Appends one frame per row in `rows` (each row's *current* in-memory
 * state - a live row's full column values, or a tombstone if the row is
 * now dead) to the WAL file at wal_path, creating it if absent, and
 * fsyncs before returning. This is the durable step for a commit that
 * touched this table: a small bounded append instead of a whole-table
 * dump_csv rewrite. Frames encode absolute final state per row (not a
 * delta), so replaying a frame twice is harmless. */
bool wal_append(const char *wal_path, const Table *t, const RowRef *rows, size_t n_rows);

/* Replays every well-formed frame in wal_path (a no-op, returning true,
 * if the file doesn't exist) onto an already-loaded table, in append
 * order. A torn/partial frame - the signature of a crash mid-append -
 * is detected and stops replay right there rather than failing; whatever
 * was applied before it stands. */
bool wal_replay(const char *wal_path, Table *t);

/* Folds wal_path's frames into the base table file via dump_csv, then
 * removes the WAL - the periodic maintenance step that keeps the WAL
 * from growing without bound. dump_csv compacts away tombstoned rows as
 * it writes, which renumbers every live row - so this also bumps the
 * <csv_path>.gen sidecar (see wal_read_generation/wal_write_generation)
 * and updates t->wal_generation to match, so this process's own next
 * commit is correctly considered fresh. t is mutated for exactly that
 * reason (not just read, as a plain checkpoint would only need). */
bool wal_checkpoint(const char *csv_path, const char *wal_path, Table *t);

/* Reads csv_path's ".gen" sidecar - the generation stamped by the most
 * recent wal_checkpoint of this table (0 if the file doesn't exist, i.e.
 * never checkpointed). Returns false only on a genuine failure to even
 * form the sidecar's path (e.g. csv_path too long); a missing or corrupt
 * sidecar file still succeeds with *out_gen set to 0, since "there's no
 * record of a checkpoint" and "there's a record showing generation 0" mean
 * the same thing to a caller comparing against a freshly-loaded table's own
 * wal_generation. */
bool wal_read_generation(const char *csv_path, uint32_t *out_gen);

/* Atomically (temp file + fsync + rename, same pattern as dump_csv) writes
 * gen to csv_path's ".gen" sidecar. */
bool wal_write_generation(const char *csv_path, uint32_t gen);

#endif
