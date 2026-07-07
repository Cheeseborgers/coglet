#ifndef COG_ARENA_H
#define COG_ARENA_H

#include <stddef.h>

// A bump allocator: hands out pointers into a big block by just
// moving a cursor forward. Nothing is ever individually freed --
// the whole arena is destroyed at once when the compilation is done.
// This is why AST nodes never need free()-ing one at a time.
typedef struct ArenaBlock ArenaBlock;

typedef struct {
    ArenaBlock *first;
    ArenaBlock *current;
    size_t block_size;
} Arena;

Arena *arena_create(size_t block_size);
void *arena_alloc(Arena *arena, size_t size);
void arena_destroy(Arena *arena);

#endif