#ifndef LOCK_H
#define LOCK_H

#include <stdbool.h>

/* An advisory, file-backed reader/writer lock keyed by a table's backing
 * path (via "<path>.lock") - the coordination mechanism M7 asks for
 * between separate db4 processes sharing the same CSV+WAL files (this
 * project has no in-process connection/thread model yet - see M8).
 * Shared locks (readers taking a snapshot in `.load`) can be held
 * concurrently with each other; an exclusive lock (a writer's WAL append,
 * or a checkpoint's base-file rewrite) excludes everyone else until
 * released. Both are held only briefly - around the read or the append/
 * checkpoint itself - never across a whole session. */
typedef struct {
    int fd;
} Db4Lock;

/* Opens (creating if needed) "<path>.lock" without acquiring a lock.
 * Returns false (lock->fd left at -1) on failure. */
bool db4_lock_open(Db4Lock *lock, const char *path);

/* Block until the requested lock is held. */
bool db4_lock_shared(Db4Lock *lock);
bool db4_lock_exclusive(Db4Lock *lock);

void db4_lock_release(Db4Lock *lock);
void db4_lock_close(Db4Lock *lock);

#endif
