#include "arena.h"
#include "budget.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#define HOG_FACTOR 4

// Records the first failure and keeps it. Later failures don't overwrite
// the first: the earliest one is the one that explains everything after
// it, since a latched arena refuses every subsequent allocation.
static void latch(Arena *a, ArenaFailure f) {
    if (a->failure == ARENA_OK) a->failure = f;
}

// A caller contract violation (bad alignment, and the equivalents in
// table.c). It's a bug in the program, not a runtime condition, so a
// debug build stops at the assert with the message; a release build has
// to keep the process alive for the host, so it latches and lets the
// operation fail like any other.
static void misuse(Arena *a, const char *msg) {
#ifndef NDEBUG
    fprintf(stderr, "db3: %s\n", msg);
    assert(0 && "arena misuse");
#endif
    (void)msg;
    latch(a, ARENA_FAIL_MISUSE);
}

static int is_power_of_two(size_t n) {
    return n != 0 && (n & (n - 1)) == 0;
}

static int checked_add_size(size_t a, size_t b, size_t *out) {
    if (a > SIZE_MAX - b) return 1;
    *out = a + b;
    return 0;
}

static int checked_mul_size(size_t a, size_t b, size_t *out) {
    if (a != 0 && b > SIZE_MAX / a) return 1;
    *out = a * b;
    return 0;
}

size_t arena_checked_add(Arena *a, size_t x, size_t y) {
    size_t out;
    if (checked_add_size(x, y, &out)) {
        latch(a, ARENA_FAIL_OOM);
        return 0;
    }
    return out;
}

size_t arena_checked_mul(Arena *a, size_t x, size_t y) {
    size_t out;
    if (checked_mul_size(x, y, &out)) {
        latch(a, ARENA_FAIL_OOM);
        return 0;
    }
    return out;
}

// Total malloc size for a block of `capacity` usable bytes, or 0 if that
// sum overflows (which latches, so the caller's allocation fails anyway).
static size_t block_alloc_size(Arena *a, size_t capacity) {
    size_t total;
    if (checked_add_size(sizeof(Block), capacity, &total)) {
        latch(a, ARENA_FAIL_OOM);
        return 0;
    }
    return total;
}

// Returns NULL (having latched) if the size math overflowed, the budget
// refused the charge, or malloc failed.
static Block *new_block(Arena *a, size_t capacity) {
    size_t total = block_alloc_size(a, capacity);
    if (total == 0) return NULL;

    // Charge the memory budget before asking the OS for it: budget_charge
    // refuses if this block would push db3 past its cap, so a runaway .load
    // can't drive the machine into thrash/OOM-kill. bytes_allocd tracks the
    // exact sum of these totals per arena, so arena_term releases precisely
    // what was charged here.
    if (!budget_charge(total)) {
        latch(a, ARENA_FAIL_OOM);
        return NULL;
    }
    Block *b = malloc(total);
    if (!b) {
        budget_uncharge(total);  // never taken up, so give it straight back
        latch(a, ARENA_FAIL_OOM);
        return NULL;
    }
    b->next     = NULL;
    b->capacity = capacity;
    b->used     = 0;
    return b;
}

static size_t next_block_capacity(Arena *a, size_t size, size_t align) {
    size_t needed;
    size_t target;
    size_t cap = a->block_size;

    if (checked_add_size(size, align - 1, &needed)) {
        latch(a, ARENA_FAIL_OOM);
        return 0;
    }
    if (checked_mul_size(needed, HOG_FACTOR, &target)) target = needed;

    while (cap < target) {
        if (cap > SIZE_MAX / 2) {
            cap = target;
            break;
        }
        cap *= 2;
    }

    return cap;
}

void arena_init(Arena *a) {
    a->block_size   = ARENA_BLOCK_SIZE;
    a->bytes_used   = 0;
    a->bytes_allocd = 0;
    a->failure      = ARENA_OK;

    // arena_init can't report failure - it's called from constructors that
    // return void - so a first block it can't get leaves the arena empty
    // and latched. The arena stays structurally valid: arena_alloc handles
    // a NULL head, and once the latch is taken it retries the first block.
    a->first = new_block(a, ARENA_BLOCK_SIZE);
    a->head  = a->first;
    if (a->first) a->bytes_allocd = sizeof(Block) + ARENA_BLOCK_SIZE;
}

void arena_term(Arena *a) {
    Block *curr = a->first;
    while (curr) {
        Block *next = curr->next;
        free(curr);
        curr = next;
    }
    // Release everything new_block charged for this arena. bytes_allocd is the
    // running sum of every block's block_alloc_size (arena_reset keeps blocks,
    // so it never changed it), which is exactly what was charged.
    budget_uncharge(a->bytes_allocd);
    a->first        = NULL;
    a->head         = NULL;
    a->bytes_allocd = 0;
}

void arena_reset(Arena *a) {
    Block *curr = a->first;
    while (curr) {
        curr->used = 0;
        curr = curr->next;
    }
    a->head       = a->first;
    a->bytes_used = 0;
}

void *arena_alloc(Arena *a, size_t size, size_t align) {
    if (!is_power_of_two(align)) {
        misuse(a, "arena_alloc alignment must be a power of two");
        return NULL;
    }
    // A latched arena refuses everything without touching the heap, so a
    // caller that runs a chain of allocations before checking does no work
    // after the first failure.
    if (a->failure != ARENA_OK) return NULL;

    while (1) {
        Block *b = a->head;
        if (!b) {
            // Empty arena: either arena_init couldn't get the first block
            // and the latch has since been taken, or arena_term ran. Try
            // again from scratch.
            Block *nb = new_block(a, a->block_size);
            if (!nb) return NULL;
            a->first = nb;
            a->head  = nb;
            // Can't overflow: new_block just computed the same sum.
            a->bytes_allocd += block_alloc_size(a, a->block_size);
            continue;
        }

        uintptr_t cur     = (uintptr_t)(b->data + b->used);
        uintptr_t aligned = (cur + align - 1) & ~(align - 1);
        size_t    padding = (size_t)(aligned - cur);

        // b->used <= b->capacity always holds, so compute the remaining space
        // and test against it without ever forming b->used + padding + size,
        // which would wrap for a `size` within `align` of SIZE_MAX and falsely
        // report a fit.
        size_t avail = b->capacity - b->used;
        if (padding <= avail && size <= avail - padding) {
            char *ptr = b->data + b->used + padding;
            b->used       += padding + size;
            a->bytes_used += padding + size;
            return ptr;
        }

        if (b->next) {
            a->head = b->next;
        } else {
            size_t cap = next_block_capacity(a, size, align);
            if (cap == 0) return NULL;
            size_t allocd = block_alloc_size(a, cap);
            if (allocd == 0) return NULL;
            Block *nb = new_block(a, cap);
            // The block that couldn't be had is simply not linked in; the
            // arena is unchanged apart from the latch, so a later retry
            // starts from exactly the state it was in before.
            if (!nb) return NULL;
            b->next         = nb;
            a->head         = nb;
            a->block_size   = cap;
            if (checked_add_size(a->bytes_allocd, allocd, &a->bytes_allocd)) {
                latch(a, ARENA_FAIL_OOM);
                return NULL;
            }
        }
    }
}

void *arena_zalloc(Arena *a, size_t size, size_t align) {
    void *ptr = arena_alloc(a, size, align);
    if (!ptr) return NULL;
    memset(ptr, 0, size);
    return ptr;
}

void *arena_grow(Arena *a, void *ptr, size_t old_size, size_t new_size, size_t align) {
    void *out = arena_alloc(a, new_size, align);
    if (!out) return NULL;

    // A NULL `ptr` is a legal "nothing to copy yet" - either the first
    // growth of an empty array, or a caller passing along a previous
    // failure - so treat it as zero old bytes rather than reading it.
    size_t copy_size = ptr ? (old_size < new_size ? old_size : new_size) : 0;
    if (copy_size) memcpy(out, ptr, copy_size);
    if (new_size > copy_size)
        memset((char *)out + copy_size, 0, new_size - copy_size);
    return out;
}

void *arena_memdup(Arena *a, const void *data, size_t len, size_t align) {
    if (!data) return NULL;
    void *out = arena_alloc(a, len, align);
    if (!out) return NULL;
    memcpy(out, data, len);
    return out;
}

char *arena_strndup(Arena *a, const char *str, size_t len) {
    if (!str) return NULL;

    size_t copy_len = 0;
    while (copy_len < len && str[copy_len] != '\0')
        copy_len++;

    char *out = arena_alloc(a, copy_len + 1, _Alignof(char));
    if (!out) return NULL;
    memcpy(out, str, copy_len);
    out[copy_len] = '\0';
    return out;
}

char *arena_strdup(Arena *a, const char *str) {
    if (!str) return NULL;
    return arena_strndup(a, str, strlen(str));
}

void arena_fail(Arena *a, ArenaFailure f) {
    latch(a, f);
}

ArenaFailure arena_failure(const Arena *a) {
    return a->failure;
}

bool arena_failed(const Arena *a) {
    return a->failure != ARENA_OK;
}

ArenaFailure arena_take_failure(Arena *a) {
    ArenaFailure f = a->failure;
    a->failure = ARENA_OK;
    return f;
}
