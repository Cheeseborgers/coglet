#include "arena.h"
#include <stdlib.h>
#include <stdio.h>

// One block is a flat byte buffer plus how much of it is used.
// When a block fills up, we allocate a new block and link to it --
// existing pointers stay valid because we never move/realloc memory
// that's already been handed out.
struct ArenaBlock {

    ArenaBlock *next;
    size_t used;
    size_t capacity;
    unsigned char data[]; // flexible array member: the actual storage
};

static ArenaBlock *block_create(size_t capacity) {

    ArenaBlock *block = malloc(sizeof(ArenaBlock) + capacity);
    if (!block) { fprintf(stderr, "arena: out of memory\n"); exit(1); }
    block->next = NULL;
    block->used = 0;
    block->capacity = capacity;
    return block;
}

Arena *arena_create(size_t block_size) {

    Arena *arena = malloc(sizeof(Arena));
    if (!arena) { fprintf(stderr, "arena: out of memory\n"); exit(1); }
    arena->block_size = block_size;
    arena->first = block_create(block_size);
    arena->current = arena->first;
    return arena;
}

// Round up to keep every allocation pointer-aligned. Without this,
// structs containing e.g. a `double` could end up misaligned on some
// platforms, which is undefined behavior.
static size_t align_up(size_t n) {

    const size_t align = sizeof(void *);
    return (n + align - 1) & ~(align - 1);
}

void *arena_alloc(Arena *arena, size_t size) {

    size = align_up(size);
    ArenaBlock *block = arena->current;

    if (block->used + size > block->capacity) {
        // Current block is full: allocate a new one, sized to fit
        // whatever's being requested even if it's bigger than usual.
        size_t new_capacity = arena->block_size > size ? arena->block_size : size;
        ArenaBlock *new_block = block_create(new_capacity);
        block->next = new_block;
        arena->current = new_block;
        block = new_block;
    }

    void *ptr = block->data + block->used;
    block->used += size;
    return ptr;
}

void arena_destroy(Arena *arena) {

    ArenaBlock *block = arena->first;

    while (block) {
        ArenaBlock *next = block->next;
        free(block);
        block = next;
    }

    free(arena);
}
