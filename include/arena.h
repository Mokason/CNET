#ifndef ARENA_H
#define ARENA_H

#include <stdalign.h>
#include <stddef.h>
#include <stdlib.h>

typedef struct {
    unsigned char *base;
    size_t used;
    size_t capacity;
} Arena;

static inline void arena_init(Arena *arena) {
    if (arena == NULL) {
        return;
    }
    arena->base = NULL;
    arena->used = 0;
    arena->capacity = 0;
}

static inline void arena_reset(Arena *arena) {
    if (arena == NULL) {
        return;
    }
    free(arena->base);
    arena->base = NULL;
    arena->used = 0;
    arena->capacity = 0;
}

static inline void *arena_alloc(Arena *arena, size_t bytes) {
    size_t align_mask;
    size_t aligned_bytes;
    size_t required;
    size_t grow_cap;
    unsigned char *result;

    if (arena == NULL) {
        return NULL;
    }
    if (bytes == 0) {
        return arena->base == NULL ? NULL : arena->base + arena->used;
    }

    align_mask = alignof(max_align_t) - 1;
    if (bytes > (size_t)-1 - align_mask) {
        return NULL;
    }
    aligned_bytes = (bytes + align_mask) & ~align_mask;
    if (arena->used > (size_t)-1 - aligned_bytes) {
        return NULL;
    }
    required = arena->used + aligned_bytes;

    if (required > arena->capacity) {
        grow_cap = arena->capacity == 0 ? 1024 : arena->capacity;
        while (grow_cap < required) {
            if (grow_cap > (size_t)-1 / 2) {
                grow_cap = required;
                break;
            }
            grow_cap *= 2;
        }

        result = realloc(arena->base, grow_cap);
        if (result == NULL) {
            return NULL;
        }
        arena->base = result;
        arena->capacity = grow_cap;
    }

    result = arena->base + arena->used;
    arena->used = required;

    return result;
}

#endif
