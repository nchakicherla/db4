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

typedef enum {
    ARENA_OK = 0,
    ARENA_FAIL_OOM,
    ARENA_FAIL_MISUSE,
} ArenaFailure;

typedef struct {
    Block *head;
    Block *first;
    size_t block_size;
    size_t bytes_used;
    size_t bytes_allocd;
    ArenaFailure failure;
} Arena;

#define arena_alloc_type(a, T) arena_alloc(a, sizeof(T), _Alignof(T))

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

void arena_fail(Arena *a, ArenaFailure f);

ArenaFailure arena_failure(const Arena *a);
bool         arena_failed(const Arena *a);

ArenaFailure arena_take_failure(Arena *a);

#endif
