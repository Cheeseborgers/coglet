#ifndef COGLET_TOOLCHAIN_NATIVE_TOOLCHAIN_H
#define COGLET_TOOLCHAIN_NATIVE_TOOLCHAIN_H

#include <stddef.h>

typedef enum CogNativeToolchainStatus {
    COG_NATIVE_TOOLCHAIN_OK = 0,
    COG_NATIVE_TOOLCHAIN_IO_ERROR,
    COG_NATIVE_TOOLCHAIN_PROCESS_ERROR,
} CogNativeToolchainStatus;

typedef struct CogNativeToolchainLinkOptions {
    const char *const *runtime_sources;
    int runtime_source_count;
    int runtime_math;

    const char *const *library_dirs;
    int library_dir_count;

    const char *const *libraries;
    int library_count;
} CogNativeToolchainLinkOptions;

/*
 * Creates an empty native temporary file and returns its path. `suffix` may be
 * empty; when non-empty it becomes part of the final path (for example `.c` or
 * `.obj`). The caller owns only the filesystem entry, not heap memory.
 */
int cog_native_create_temp_file(
    char *path,
    size_t path_capacity,
    const char *stem,
    const char *suffix
);

void cog_native_remove_file(const char *path);

/* Compile one generated C translation unit plus an optional Coglet runtime C
 * source and link an executable with the configured native C toolchain. */
CogNativeToolchainStatus cog_native_build_c_executable(
    const char *output_path,
    const char *generated_c_path,
    const CogNativeToolchainLinkOptions *options
);

/* Link one already-emitted native object plus an optional runtime C source. */
CogNativeToolchainStatus cog_native_link_object_executable(
    const char *output_path,
    const char *object_path,
    const CogNativeToolchainLinkOptions *options
);

#endif
