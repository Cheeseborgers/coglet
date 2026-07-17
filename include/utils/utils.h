#ifndef COGLET_UTILS_H
#define COGLET_UTILS_H

#include <stdlib.h>
#include <stdio.h>

#include "arena.h"

// TODO: Consider moving this elsewhere
#define MULTI_THREADING 0

#define KB(x) ((size_t)(x) * 1024ULL)
#define MB(x) (KB(x) * 1024ULL)
#define GB(x) (MB(x) * 1024ULL)
#define TB(x) (GB(x) * 1024ULL)

#define UNUSED(x) (void)(x)
#define NOOP(x)   (void)(x)

#define TODO(message)                                                                                                  \
do {                                                                                                                   \
fprintf(stderr, "%s:%d: TODO: %s\n", __FILE__, __LINE__, message);                                                     \
abort();                                                                                                               \
} while (0)

#define UNREACHABLE(message)                                                                                           \
do {                                                                                                                   \
fprintf(stderr, "%s:%d: UNREACHABLE: %s\n", __FILE__, __LINE__, message);                                              \
abort();                                                                                                               \
} while (0)

char *read_file(const char *path);

/*
 * Reads an entire file into arena-owned, null-terminated memory.
 *
 * Returns NULL on an open, seek, size, or read failure.
 * The returned buffer remains valid until the arena is reset past the
 * allocation or destroyed.
 */
char *arena_read_file(Arena *arena, const char *path);

#endif