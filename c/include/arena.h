#ifndef ARENA_H
#define ARENA_H

#include <stdbool.h>
#include <stddef.h>

#define ARENA_BLOCK_SIZE 4096

typedef struct Block {
    struct Block *next;
    size_t        capacity;
    size_t        used;
    char          data[];
} Block;

// Why the arena latches instead of aborting.
//
// A library can't call exit() on its host, but making every allocation
// site return a status would put an error check on ~40 call sites whose
// only sensible reaction is "give up on this operation". So the arena
// records its first failure and keeps it: once an arena has failed, every
// subsequent allocation returns NULL immediately without touching the
// heap, and the caller checks the latch once, at the boundary of a whole
// operation, instead of after each allocation.
//
// That leaves two rules for callers, both of which the arena's
// never-free, copy-on-grow design makes cheap:
//
//   1. Allocate into a local, not directly into the structure being
//      built. A failed arena_grow returns NULL; assigning that over a
//      live pointer would lose the old (still valid) buffer.
//
//   2. Commit the size/count field last. A structure whose backing arrays
//      grew but whose count never advanced is exactly the structure it
//      was before - the extra bytes stay charged and unreachable, which
//      is the price of not unwinding.
//
// A NULL return must never be dereferenced, but every arena_* function
// tolerates a NULL `ptr`/`str`/`data` input, so a chain of allocations
// can run to its end and be checked once rather than after each step.
typedef enum {
    ARENA_OK = 0,
    ARENA_FAIL_OOM,     // malloc failed, budget refused, or a size computation overflowed
    ARENA_FAIL_MISUSE,  // caller contract violation; also asserts in a debug build
} ArenaFailure;

typedef struct {
    Block *head;
    Block *first;
    size_t block_size;
    size_t bytes_used;
    size_t bytes_allocd;
    // First failure seen, sticky until taken. See the comment above.
    ArenaFailure failure;
} Arena;

#define arena_alloc_type(a, T) arena_alloc(a, sizeof(T), _Alignof(T))

// Returns x+y / x*y, latching ARENA_FAIL_OOM and returning 0 on size_t
// overflow instead of silently wrapping. For callers doing their own
// buffer-growth math outside the arena (e.g. table.c's heap/column
// growth), so that math gets the same overflow discipline as the arena's.
// A 0 result is safe to feed onward: the allocation it sizes will be
// refused by the already-latched arena anyway.
size_t arena_checked_add(Arena *a, size_t x, size_t y);
size_t arena_checked_mul(Arena *a, size_t x, size_t y);

void  arena_init(Arena *a);
void  arena_term(Arena *a);
void  arena_reset(Arena *a);
void *arena_alloc(Arena *a, size_t size, size_t align);
void *arena_zalloc(Arena *a, size_t size, size_t align);
void *arena_grow(Arena *a, void *ptr, size_t old_size, size_t new_size, size_t align);
void *arena_memdup(Arena *a, const void *data, size_t len, size_t align);
char *arena_strdup(Arena *a, const char *str);
char *arena_strndup(Arena *a, const char *str, size_t len);

// Latches a failure the caller detected itself - a size that can't be
// represented in the structure this arena backs, a violated precondition -
// so it surfaces through the same single check as an allocation failure
// rather than needing a second channel. The first failure wins.
void arena_fail(Arena *a, ArenaFailure f);

// Reads the latch without clearing it.
ArenaFailure arena_failure(const Arena *a);
bool         arena_failed(const Arena *a);

// Reads and clears the latch, so the arena is usable again. Callers that
// report a failure outward take it; a latch that was never cleared would
// poison the arena for the rest of the process, failing legitimate
// retries after the host had freed memory.
ArenaFailure arena_take_failure(Arena *a);

#endif
