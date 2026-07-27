#ifndef WAL_H
#define WAL_H

#include <stdbool.h>
#include <stddef.h>

#include "table.h"

/* Appends one frame per row in `rows` (each row's *current* in-memory
 * state - a live row's full column values, or a tombstone if the row is
 * now dead) to the WAL file at wal_path, creating it if absent, and
 * fsyncs before returning. This is the durable step for a commit that
 * touched this table: a small bounded append instead of a whole-table
 * dump_csv rewrite. Frames encode absolute final state per row (not a
 * delta), so replaying a frame twice is harmless. */
bool wal_append(const char *wal_path, const Table *t, const size_t *rows, size_t n_rows);

/* Replays every well-formed frame in wal_path (a no-op, returning true,
 * if the file doesn't exist) onto an already-loaded table, in append
 * order. A torn/partial frame - the signature of a crash mid-append -
 * is detected and stops replay right there rather than failing; whatever
 * was applied before it stands. */
bool wal_replay(const char *wal_path, Table *t);

/* Folds wal_path's frames into the base table file via dump_csv, then
 * removes the WAL - the periodic maintenance step that keeps the WAL
 * from growing without bound. */
bool wal_checkpoint(const char *csv_path, const char *wal_path, const Table *t);

#endif
