#ifndef COGLET_UTILS_H
#define COGLET_UTILS_H

#include <stdlib.h>
#include <stdio.h>



// TODO: Consider moving these elsewhere
#define MULTI_THREADING 0

#define EXIT_OK 0
#define EXIT_SEMANTIC_ERROR 1
#define EXIT_PARSE_ERROR 2
#define EXIT_DRIVER_ERROR 2

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

#endif