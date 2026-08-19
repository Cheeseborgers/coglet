#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

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


static char *coglet_rt_io_path_buffer(const uint8_t *path, size_t length)
{
    char *buffer = (char *)malloc(length + 1);
    if (!buffer)
        return NULL;

    if (length != 0)
        memcpy(buffer, path, length);
    buffer[length] = '\0';
    return buffer;
}

_Bool coglet_rt_io_file_seek(void *handle, int64_t offset, int32_t origin)
{
    int whence;

    if (!handle)
        return 0;

    switch (origin) {
    case 0: whence = SEEK_SET; break;
    case 1: whence = SEEK_CUR; break;
    case 2: whence = SEEK_END; break;
    default: return 0;
    }

#ifdef _WIN32
    return _fseeki64((FILE *)handle, offset, whence) == 0;
#else
    return fseek((FILE *)handle, (long)offset, whence) == 0;
#endif
}

int64_t coglet_rt_io_file_tell(void *handle)
{
    if (!handle)
        return -1;

#ifdef _WIN32
    return (int64_t)_ftelli64((FILE *)handle);
#else
    return (int64_t)ftell((FILE *)handle);
#endif
}

_Bool coglet_rt_io_file_set_buffered(void *handle, _Bool buffered)
{
    if (!handle)
        return 0;

    return setvbuf(
        (FILE *)handle,
        NULL,
        buffered ? _IOFBF : _IONBF,
        buffered ? BUFSIZ : 0
    ) == 0;
}

_Bool coglet_rt_io_file_exists(const uint8_t *path, size_t length)
{
    char *buffer;
    int result;

    buffer = coglet_rt_io_path_buffer(path, length);
    if (!buffer)
        return 0;

#ifdef _WIN32
    {
        struct _stat64 info;
        result = _stat64(buffer, &info);
    }
#else
    {
        struct stat info;
        result = stat(buffer, &info);
    }
#endif

    free(buffer);
    return result == 0;
}

_Bool coglet_rt_io_file_copy(
    const uint8_t *source,
    size_t source_length,
    const uint8_t *destination,
    size_t destination_length)
{
    char *source_path = NULL;
    char *destination_path = NULL;
    FILE *input = NULL;
    FILE *output = NULL;
    uint8_t buffer[8192];
    size_t count;
    _Bool success = 0;

    source_path = coglet_rt_io_path_buffer(source, source_length);
    destination_path = coglet_rt_io_path_buffer(destination, destination_length);
    if (!source_path || !destination_path)
        goto cleanup;

    input = fopen(source_path, "rb");
    if (!input)
        goto cleanup;

    output = fopen(destination_path, "wb");
    if (!output)
        goto cleanup;

    while ((count = fread(buffer, 1, sizeof(buffer), input)) != 0) {
        if (fwrite(buffer, 1, count, output) != count)
            goto cleanup;
    }

    success = ferror(input) == 0 && fflush(output) == 0;

cleanup:
    if (output)
        fclose(output);
    if (input)
        fclose(input);
    free(destination_path);
    free(source_path);
    return success;
}

_Bool coglet_rt_io_file_rename(
    const uint8_t *source,
    size_t source_length,
    const uint8_t *destination,
    size_t destination_length)
{
    char *source_path = coglet_rt_io_path_buffer(source, source_length);
    char *destination_path = coglet_rt_io_path_buffer(destination, destination_length);
    int result;

    if (!source_path || !destination_path) {
        free(destination_path);
        free(source_path);
        return 0;
    }

    result = rename(source_path, destination_path);
    free(destination_path);
    free(source_path);
    return result == 0;
}

_Bool coglet_rt_io_directory_create(const uint8_t *path, size_t length)
{
    char *buffer = coglet_rt_io_path_buffer(path, length);
    int result;

    if (!buffer)
        return 0;

#ifdef _WIN32
    result = _mkdir(buffer);
#else
    result = mkdir(buffer, 0777);
#endif

    free(buffer);
    return result == 0;
}

_Bool coglet_rt_io_directory_remove(const uint8_t *path, size_t length)
{
    char *buffer = coglet_rt_io_path_buffer(path, length);
    int result;

    if (!buffer)
        return 0;

#ifdef _WIN32
    result = _rmdir(buffer);
#else
    result = rmdir(buffer);
#endif

    free(buffer);
    return result == 0;
}

typedef struct CogletDirectory {
#ifdef _WIN32
    HANDLE handle;
    WIN32_FIND_DATAA entry;
    _Bool first;
#else
    DIR *handle;
#endif
} CogletDirectory;

void *coglet_rt_io_directory_open(const uint8_t *path, size_t length)
{
    char *buffer = coglet_rt_io_path_buffer(path, length);
    CogletDirectory *directory;

    if (!buffer)
        return NULL;

    directory = (CogletDirectory *)calloc(1, sizeof(CogletDirectory));
    if (!directory) {
        free(buffer);
        return NULL;
    }

#ifdef _WIN32
    {
        size_t path_length = strlen(buffer);
        char *pattern = (char *)malloc(path_length + 3);

        if (!pattern) {
            free(directory);
            free(buffer);
            return NULL;
        }

        memcpy(pattern, buffer, path_length);
        if (path_length != 0 && pattern[path_length - 1] != '\\' &&
            pattern[path_length - 1] != '/')
            pattern[path_length++] = '\\';
        pattern[path_length++] = '*';
        pattern[path_length] = '\0';

        directory->handle = FindFirstFileA(pattern, &directory->entry);
        directory->first = 1;
        free(pattern);
        if (directory->handle == INVALID_HANDLE_VALUE) {
            free(directory);
            free(buffer);
            return NULL;
        }
    }
#else
    directory->handle = opendir(buffer);
    if (!directory->handle) {
        free(directory);
        free(buffer);
        return NULL;
    }
#endif

    free(buffer);
    return directory;
}

_Bool coglet_rt_io_directory_next(
    void *handle,
    uint8_t *name,
    size_t capacity,
    size_t *out_length)
{
    CogletDirectory *directory = (CogletDirectory *)handle;

    if (!directory || !name || !out_length || capacity == 0)
        return 0;

    for (;;) {
        const char *entry_name;

#ifdef _WIN32
        if (directory->first) {
            directory->first = 0;
        } else if (!FindNextFileA(directory->handle, &directory->entry)) {
            return 0;
        }
        entry_name = directory->entry.cFileName;
#else
        {
            struct dirent *entry = readdir(directory->handle);
            if (!entry)
                return 0;
            entry_name = entry->d_name;
        }
#endif

        if (strcmp(entry_name, ".") == 0 || strcmp(entry_name, "..") == 0)
            continue;

        {
            size_t length = strlen(entry_name);
            if (length >= capacity)
                return 0;

            memcpy(name, entry_name, length);
            *out_length = length;
            return 1;
        }
    }
}

void coglet_rt_io_directory_close(void *handle)
{
    CogletDirectory *directory = (CogletDirectory *)handle;

    if (!directory)
        return;

#ifdef _WIN32
    if (directory->handle != INVALID_HANDLE_VALUE)
        FindClose(directory->handle);
#else
    if (directory->handle)
        closedir(directory->handle);
#endif

    free(directory);
}
