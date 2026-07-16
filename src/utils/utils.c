#include "../../include/utils/utils.h"

#include <stdio.h>
#include <stdlib.h>

// TODO: Consider moving these elsewhere
#define EXIT_OK 0
#define EXIT_SEMANTIC_ERROR 1
#define EXIT_PARSE_ERROR 2
#define EXIT_DRIVER_ERROR 2

char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    char *buf = malloc(size + 1);
    fread(buf, 1, size, f);
    buf[size] = '\0';

    fclose(f);
    return buf;
}