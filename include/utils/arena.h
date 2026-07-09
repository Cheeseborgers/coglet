#ifndef COGLET_ARENA_H
#define COGLET_ARENA_H

#include <stddef.h>

/*
===============================================================================
Arena Allocator
===============================================================================

Typical uses:
    - AST nodes
    - Tokens
    - Parser data structures
    - Symbol tables
    - Temporary compiler allocations

Advantages
----------
- Allocation is O(1) in the common case.
- No fragmentation.
- Individual objects never need free().
- Existing pointers remain valid for the lifetime of the allocation.
- The arena grows automatically by allocating additional blocks.

Limitations
-----------
- Individual allocations cannot be freed.
- Memory is reclaimed only by:
      arena_reset_to()
      arena_destroy()
- Not thread-safe.
- Memory is uninitialised.

Marker API
----------
A marker records the current allocation position.

    ArenaMarker m = arena_mark(arena);

Allocations made after the marker can later be discarded:

    arena_reset_to(arena, m);

After reset:
- All pointers allocated after the marker become invalid.
- Pointers allocated before the marker remain valid.

Behaviour is undefined if:
- A marker from one arena is used with another arena.
- A marker is used after its arena has been destroyed.
- The arena has already been reset to an earlier point that invalidated the
  marker.

Alignment
---------
Returned pointers are aligned to at least sizeof(void *), making them suitable
for storing any normal C object.

Statistics
----------
arena_allocated()
    Returns the number of bytes currently allocated.

arena_remaining()
    Returns the remaining free space in the current block before another block
    must be allocated. This is *not* the remaining capacity of the entire
    arena.

===============================================================================
*/

typedef struct ArenaBlock ArenaBlock;

typedef struct {
    ArenaBlock *first;
    ArenaBlock *current;

    /* Default size for newly allocated blocks. */
    size_t block_size;

    /* Total bytes currently allocated. */
    size_t allocated;
} Arena;

/*
 * Opaque snapshot of an arena's allocation state.
 *
 * Create with arena_mark() and restore with arena_reset_to().
 * Users should treat this as an opaque value and never modify its members.
 */
typedef struct {
    ArenaBlock *block;
    size_t used;
} ArenaMarker;

/*
 * Creates a new arena.
 *
 * block_size specifies the default size (in bytes) of internal blocks.
 * Larger individual allocations automatically receive a suitably sized block.
 *
 * Returns:
 *      Pointer to the new arena.
 *
 * Exits the program if memory cannot be allocated.
 */
Arena *arena_create(size_t block_size);

/*
 * Allocates size bytes from the arena.
 *
 * Memory is:
 *      - suitably aligned
 *      - uninitialised
 *      - valid until arena_reset_to() or arena_destroy()
 *
 * Never returns NULL. The program exits on allocation failure.
 */
void *arena_alloc(Arena *arena, size_t size);

/*
 * Destroys the arena and frees every allocation made through it.
 *
 * Every pointer obtained from arena_alloc() becomes invalid.
 */
void arena_destroy(Arena *arena);

/*
 * Saves the current allocation position.
 */
ArenaMarker arena_mark(const Arena *arena);

/*
 * Restores the arena to a previously saved marker.
 *
 * All allocations performed after the marker are discarded.
 *
 * Undefined behaviour if mark was not created by arena_mark() on this arena.
 */
void arena_reset_to(Arena *arena, ArenaMarker mark);

/*
 * Returns the number of bytes currently allocated.
 */
size_t arena_allocated(const Arena *arena);

/*
 * Returns the unused space remaining in the current block.
 *
 * This value reaches zero immediately before a new block would be allocated.
 */
size_t arena_remaining(const Arena *arena);

#endif