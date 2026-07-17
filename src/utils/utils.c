#include "utils/utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

char *read_file(const char *path)
{
    FILE *file = fopen(path, "rb");

    if (!file) return NULL;

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }

    long file_size = ftell(file);

    if (file_size < 0) {
        fclose(file);
        return NULL;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    size_t size  = (size_t)file_size;
    char *source = malloc(size + 1);

    if (!source) {
        fclose(file);
        return NULL;
    }

    size_t bytes_read = fread(
        source,
        1,
        size,
        file
    );

    if (bytes_read != size) {
        free(source);
        fclose(file);
        return NULL;
    }

    source[size] = '\0';

    fclose(file);
    return source;
}

char *arena_read_file(Arena *arena, const char *path)
{
    if (!arena || !path) return NULL;

    FILE *file = fopen(path, "rb");

    if (!file) return NULL;

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }

    long file_size = ftell(file);

    if (file_size < 0) {
        fclose(file);
        return NULL;
    }

    if ((uintmax_t)file_size > (uintmax_t)SIZE_MAX - 1) {
        fclose(file);
        return NULL;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    size_t size = (size_t)file_size;

    /*
     * Restore the arena if the subsequent read fails. This is mostly
     * defensive for ordinary source files, but gives the function a
     * clean failure contract.
     */
    ArenaMarker marker = arena_mark(arena);

    char *source = arena_alloc(arena, size + 1);

    size_t bytes_read = fread(
        source,
        1,
        size,
        file
    );

    fclose(file);

    if (bytes_read != size) {
        arena_reset_to(arena, marker);
        return NULL;
    }

    source[size] = '\0';
    return source;
}