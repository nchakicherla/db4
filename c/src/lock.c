#include "lock.h"

#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <unistd.h>

bool db4_lock_open(Db4Lock *lock, const char *path) {
    char lock_path[4160];
    int  n = snprintf(lock_path, sizeof lock_path, "%s.lock", path);
    if (n < 0 || (size_t)n >= sizeof lock_path) {
        lock->fd = -1;
        return false;
    }

    lock->fd = open(lock_path, O_RDWR | O_CREAT, 0644);
    return lock->fd >= 0;
}

bool db4_lock_shared(Db4Lock *lock) {
    return lock->fd >= 0 && flock(lock->fd, LOCK_SH) == 0;
}

bool db4_lock_exclusive(Db4Lock *lock) {
    return lock->fd >= 0 && flock(lock->fd, LOCK_EX) == 0;
}

void db4_lock_release(Db4Lock *lock) {
    if (lock->fd >= 0) flock(lock->fd, LOCK_UN);
}

void db4_lock_close(Db4Lock *lock) {
    if (lock->fd >= 0) close(lock->fd);
    lock->fd = -1;
}
