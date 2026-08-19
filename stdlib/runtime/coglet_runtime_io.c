#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Coglet runtime ABI, v0.
 *
 * These symbols are implementation details used by runtime-facing standard
 * modules. User code should import std.io/std.math/std.mem rather than declaring them directly.
 * The ABI intentionally uses only ISO C scalar/pointer types so the same source
 * compiles for Linux and Windows on both x86-64 and AArch64 native toolchains.
 */

/* Standard I/O runtime component. */

void coglet_rt_io_write(const uint8_t *data, size_t length)
{
    if (length == 0)
        return;

    (void)fwrite(data, 1, (size_t)length, stdout);
}

void coglet_rt_io_newline(void)
{
    fputc('\n', stdout);
}

void coglet_rt_io_flush(void)
{
    fflush(stdout);
}

void coglet_rt_io_print_bool(_Bool value)
{
    fputs(value ? "true" : "false", stdout);
}

void coglet_rt_io_print_s8(int8_t value)
{
    fprintf(stdout, "%" PRId8, value);
}

void coglet_rt_io_print_s16(int16_t value)
{
    fprintf(stdout, "%" PRId16, value);
}

void coglet_rt_io_print_s32(int32_t value)
{
    fprintf(stdout, "%" PRId32, value);
}

void coglet_rt_io_print_s64(int64_t value)
{
    fprintf(stdout, "%" PRId64, value);
}

void coglet_rt_io_print_u8(uint8_t value)
{
    fprintf(stdout, "%" PRIu8, value);
}

void coglet_rt_io_print_u16(uint16_t value)
{
    fprintf(stdout, "%" PRIu16, value);
}

void coglet_rt_io_print_u32(uint32_t value)
{
    fprintf(stdout, "%" PRIu32, value);
}

void coglet_rt_io_print_u64(uint64_t value)
{
    fprintf(stdout, "%" PRIu64, value);
}

void coglet_rt_io_print_f32(float value)
{
    fprintf(stdout, "%.9g", (double)value);
}

void coglet_rt_io_print_f64(double value)
{
    fprintf(stdout, "%.17g", value);
}

static FILE *coglet_rt_io_file_path_open(const uint8_t *path, size_t length, const char *mode)
{
    char *buffer;
    FILE *file;

    buffer = (char *)malloc(length + 1);
    if (!buffer)
        return NULL;

    if (length != 0)
        memcpy(buffer, path, length);
    buffer[length] = '\0';
    file = fopen(buffer, mode);
    free(buffer);
    return file;
}

void *coglet_rt_io_file_open_read(const uint8_t *path, size_t length)
{
    return coglet_rt_io_file_path_open(path, length, "rb");
}

void *coglet_rt_io_file_open_write(const uint8_t *path, size_t length)
{
    return coglet_rt_io_file_path_open(path, length, "wb");
}

void *coglet_rt_io_file_open_append(const uint8_t *path, size_t length)
{
    return coglet_rt_io_file_path_open(path, length, "ab");
}

void coglet_rt_io_file_close(void *handle)
{
    if (handle)
        (void)fclose((FILE *)handle);
}

size_t coglet_rt_io_file_read(void *handle, uint8_t *data, size_t length)
{
    if (!handle || length == 0)
        return 0;
    return fread(data, 1, length, (FILE *)handle);
}

size_t coglet_rt_io_file_write(void *handle, const uint8_t *data, size_t length)
{
    if (!handle || length == 0)
        return 0;
    return fwrite(data, 1, length, (FILE *)handle);
}

_Bool coglet_rt_io_file_flush(void *handle)
{
    return handle && fflush((FILE *)handle) == 0;
}

_Bool coglet_rt_io_file_eof(void *handle)
{
    return handle && feof((FILE *)handle);
}

_Bool coglet_rt_io_file_error(void *handle)
{
    return !handle || ferror((FILE *)handle);
}

_Bool coglet_rt_io_file_remove(const uint8_t *path, size_t length)
{
    char *buffer;
    int result;

    buffer = (char *)malloc(length + 1);
    if (!buffer)
        return 0;
    if (length != 0)
        memcpy(buffer, path, length);
    buffer[length] = '\0';
    result = remove(buffer);
    free(buffer);
    return result == 0;
}
