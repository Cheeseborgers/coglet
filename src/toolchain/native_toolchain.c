#include "toolchain/native_toolchain.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coglet_paths.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

typedef struct ArgVector {
    char **items;
    size_t count;
    size_t capacity;

    char **owned;
    size_t owned_count;
    size_t owned_capacity;
} ArgVector;

static void args_destroy(ArgVector *args)
{
    if (!args)
        return;
    for (size_t i = 0; i < args->owned_count; ++i)
        free(args->owned[i]);
    free(args->owned);
    free(args->items);
    memset(args, 0, sizeof(*args));
}

static int grow_pointer_array(void ***items, size_t *capacity, size_t needed)
{
    if (*capacity >= needed)
        return 1;
    size_t next = *capacity ? *capacity * 2 : 16;
    while (next < needed)
        next *= 2;
    void **grown = realloc(*items, next * sizeof(*grown));
    if (!grown)
        return 0;
    *items = grown;
    *capacity = next;
    return 1;
}

static int args_push(ArgVector *args, const char *value)
{
    if (!grow_pointer_array((void ***)&args->items, &args->capacity, args->count + 2))
        return 0;
    args->items[args->count++] = (char *)value;
    args->items[args->count] = NULL;
    return 1;
}

#if COGLET_CONFIGURED_NATIVE_C_COMPILER_MSVC_STYLE
static int args_push_owned_concat(ArgVector *args, const char *prefix, const char *value, const char *suffix)
{
    size_t prefix_len = strlen(prefix);
    size_t value_len = strlen(value);
    size_t suffix_len = strlen(suffix);
    char *joined = malloc(prefix_len + value_len + suffix_len + 1);
    if (!joined)
        return 0;
    memcpy(joined, prefix, prefix_len);
    memcpy(joined + prefix_len, value, value_len);
    memcpy(joined + prefix_len + value_len, suffix, suffix_len + 1);

    if (!grow_pointer_array((void ***)&args->owned, &args->owned_capacity, args->owned_count + 1)) {
        free(joined);
        return 0;
    }
    args->owned[args->owned_count++] = joined;
    return args_push(args, joined);
}
#endif

static const char *temp_directory(void)
{
#if defined(_WIN32)
    const char *value = getenv("TEMP");
    if (!value || !value[0])
        value = getenv("TMP");
    return value && value[0] ? value : ".";
#else
    const char *value = getenv("TMPDIR");
    return value && value[0] ? value : "/tmp";
#endif
}

static int append_temp_template(char *path, size_t capacity, const char *stem)
{
    const char *dir = temp_directory();
    size_t dir_len = strlen(dir);
    int separator = dir_len > 0 && dir[dir_len - 1] != '/' && dir[dir_len - 1] != '\\';
#if defined(_WIN32)
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    char separator_text[2] = { sep, '\0' };
    int written = snprintf(
        path,
        capacity,
        "%s%s%s-XXXXXX",
        dir,
        separator ? separator_text : "",
        stem && stem[0] ? stem : "coglet"
    );
    return written >= 0 && (size_t)written < capacity;
}

int cog_native_create_temp_file(
    char *path,
    size_t path_capacity,
    const char *stem,
    const char *suffix
) {
    if (!path || path_capacity == 0)
        return 0;
    if (!suffix)
        suffix = "";

    char base[4096];
    if (!append_temp_template(base, sizeof(base), stem)) {
        fprintf(stderr, "error: native toolchain temporary path is too long\n");
        return 0;
    }

#if defined(_WIN32)
    if (_mktemp_s(base, strlen(base) + 1) != 0) {
        fprintf(stderr, "error: could not choose native toolchain temporary file\n");
        return 0;
    }
    int fd = _open(base, _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (fd < 0) {
        fprintf(stderr, "error: could not create native toolchain temporary file: %s\n", strerror(errno));
        return 0;
    }
    if (_close(fd) != 0) {
        _unlink(base);
        fprintf(stderr, "error: could not close native toolchain temporary file: %s\n", strerror(errno));
        return 0;
    }
#else
    int fd = mkstemp(base);
    if (fd < 0) {
        fprintf(stderr, "error: could not create native toolchain temporary file: %s\n", strerror(errno));
        return 0;
    }
    if (close(fd) != 0) {
        unlink(base);
        fprintf(stderr, "error: could not close native toolchain temporary file: %s\n", strerror(errno));
        return 0;
    }
#endif

    int written = snprintf(path, path_capacity, "%s%s", base, suffix);
    if (written < 0 || (size_t)written >= path_capacity) {
#if defined(_WIN32)
        _unlink(base);
#else
        unlink(base);
#endif
        fprintf(stderr, "error: native toolchain temporary path is too long\n");
        return 0;
    }

    if (suffix[0] && rename(base, path) != 0) {
#if defined(_WIN32)
        _unlink(base);
#else
        unlink(base);
#endif
        fprintf(stderr, "error: could not finalize native toolchain temporary file: %s\n", strerror(errno));
        return 0;
    }
    if (!suffix[0])
        memcpy(path, base, strlen(base) + 1);
    return 1;
}

void cog_native_remove_file(const char *path)
{
    if (!path || !path[0])
        return;
#if defined(_WIN32)
    _unlink(path);
#else
    unlink(path);
#endif
}

static CogNativeToolchainStatus run_process(char *const *argv)
{
    if (!argv || !argv[0])
        return COG_NATIVE_TOOLCHAIN_PROCESS_ERROR;

#if defined(_WIN32)
    intptr_t result = _spawnvp(_P_WAIT, argv[0], (const char *const *)argv);
    if (result == -1) {
        fprintf(stderr, "error: could not execute native C compiler '%s': %s\n", argv[0], strerror(errno));
        return COG_NATIVE_TOOLCHAIN_PROCESS_ERROR;
    }
    if (result != 0) {
        fprintf(stderr, "error: native C compiler/linker failed with exit %ld\n", (long)result);
        return COG_NATIVE_TOOLCHAIN_PROCESS_ERROR;
    }
    return COG_NATIVE_TOOLCHAIN_OK;
#else
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "error: could not start native C compiler: %s\n", strerror(errno));
        return COG_NATIVE_TOOLCHAIN_PROCESS_ERROR;
    }
    if (child == 0) {
        execv(argv[0], argv);
        execvp(argv[0], argv);
        fprintf(stderr, "error: could not execute native C compiler '%s': %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    int status = 0;
    if (waitpid(child, &status, 0) < 0) {
        fprintf(stderr, "error: failed waiting for native C compiler: %s\n", strerror(errno));
        return COG_NATIVE_TOOLCHAIN_PROCESS_ERROR;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "error: native C compiler/linker failed\n");
        return COG_NATIVE_TOOLCHAIN_PROCESS_ERROR;
    }
    return COG_NATIVE_TOOLCHAIN_OK;
#endif
}

#if !COGLET_CONFIGURED_NATIVE_C_COMPILER_MSVC_STYLE
static int append_gnu_link_options(ArgVector *args, const CogNativeToolchainLinkOptions *options)
{
    if (!options)
        return 1;
    for (int i = 0; i < options->library_dir_count; ++i) {
        if (!args_push(args, "-L") || !args_push(args, options->library_dirs[i]))
            return 0;
    }
    for (int i = 0; i < options->library_count; ++i) {
        if (!args_push(args, "-l") || !args_push(args, options->libraries[i]))
            return 0;
    }
    return 1;
}

#endif

#if COGLET_CONFIGURED_NATIVE_C_COMPILER_MSVC_STYLE
static int append_msvc_link_options(ArgVector *args, const CogNativeToolchainLinkOptions *options)
{
    if (!options || (options->library_dir_count == 0 && options->library_count == 0))
        return 1;

    for (int i = 0; i < options->library_count; ++i) {
        if (!args_push_owned_concat(args, "", options->libraries[i], ".lib"))
            return 0;
    }
    if (options->library_dir_count > 0 && !args_push(args, "/link"))
        return 0;
    for (int i = 0; i < options->library_dir_count; ++i) {
        if (!args_push_owned_concat(args, "/LIBPATH:", options->library_dirs[i], ""))
            return 0;
    }
    return 1;
}

#endif

#if !COGLET_CONFIGURED_NATIVE_C_COMPILER_MSVC_STYLE
static CogNativeToolchainStatus build_gnu_style(
    const char *output_path,
    const char *primary_input,
    int primary_is_c,
    const CogNativeToolchainLinkOptions *options
) {
    ArgVector args = {0};
    int ok = args_push(&args, COGLET_CONFIGURED_NATIVE_C_COMPILER);
    if (ok && primary_is_c) {
        ok = args_push(&args, "-std=c99") &&
             args_push(&args, "-Wall") &&
             args_push(&args, "-Wextra") &&
             args_push(&args, "-x") &&
             args_push(&args, "c");
    }
    if (ok)
        ok = args_push(&args, primary_input);
    if (ok && options && options->runtime_source) {
        if (!primary_is_c)
            ok = args_push(&args, "-std=c99") && args_push(&args, "-Wall") && args_push(&args, "-Wextra");
        if (ok && options->runtime_math)
            ok = args_push(&args, "-DCOGLET_RUNTIME_MATH=1");
        if (ok)
            ok = args_push(&args, options->runtime_source);
    }
    if (ok)
        ok = args_push(&args, "-o") && args_push(&args, output_path);
    if (ok)
        ok = append_gnu_link_options(&args, options);
#if !defined(_WIN32)
    if (ok && options && options->runtime_math)
        ok = args_push(&args, "-lm");
#endif

    CogNativeToolchainStatus status = COG_NATIVE_TOOLCHAIN_PROCESS_ERROR;
    if (!ok) {
        fprintf(stderr, "error: could not allocate native C compiler arguments\n");
    } else {
        status = run_process(args.items);
    }
    args_destroy(&args);
    return status;
}

#endif

#if COGLET_CONFIGURED_NATIVE_C_COMPILER_MSVC_STYLE
static CogNativeToolchainStatus build_msvc_style(
    const char *output_path,
    const char *primary_input,
    int primary_is_c,
    const CogNativeToolchainLinkOptions *options
) {
    ArgVector args = {0};
    int ok = args_push(&args, COGLET_CONFIGURED_NATIVE_C_COMPILER) &&
             args_push(&args, "/nologo") &&
             args_push(&args, "/std:c11") &&
             args_push(&args, "/W4");
    if (ok) {
        if (primary_is_c)
            ok = args_push_owned_concat(&args, "/Tc", primary_input, "");
        else
            ok = args_push(&args, primary_input);
    }
    if (ok && options && options->runtime_math)
        ok = args_push(&args, "/DCOGLET_RUNTIME_MATH=1");
    if (ok && options && options->runtime_source)
        ok = args_push_owned_concat(&args, "/Tc", options->runtime_source, "");
    if (ok)
        ok = args_push_owned_concat(&args, "/Fe:", output_path, "");
    if (ok)
        ok = append_msvc_link_options(&args, options);

    CogNativeToolchainStatus status = COG_NATIVE_TOOLCHAIN_PROCESS_ERROR;
    if (!ok) {
        fprintf(stderr, "error: could not allocate native C compiler arguments\n");
    } else {
        status = run_process(args.items);
    }
    args_destroy(&args);
    return status;
}

#endif

static CogNativeToolchainStatus build_executable(
    const char *output_path,
    const char *primary_input,
    int primary_is_c,
    const CogNativeToolchainLinkOptions *options
) {
    if (!output_path || !primary_input) {
        fprintf(stderr, "error: native toolchain requires an input and executable output path\n");
        return COG_NATIVE_TOOLCHAIN_IO_ERROR;
    }
#if COGLET_CONFIGURED_NATIVE_C_COMPILER_MSVC_STYLE
    return build_msvc_style(output_path, primary_input, primary_is_c, options);
#else
    return build_gnu_style(output_path, primary_input, primary_is_c, options);
#endif
}

CogNativeToolchainStatus cog_native_build_c_executable(
    const char *output_path,
    const char *generated_c_path,
    const CogNativeToolchainLinkOptions *options
) {
    return build_executable(output_path, generated_c_path, 1, options);
}

CogNativeToolchainStatus cog_native_link_object_executable(
    const char *output_path,
    const char *object_path,
    const CogNativeToolchainLinkOptions *options
) {
    return build_executable(output_path, object_path, 0, options);
}
