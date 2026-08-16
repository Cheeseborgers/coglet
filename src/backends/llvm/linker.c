#define _POSIX_C_SOURCE 200809L

#include "backends/llvm/backend_llvm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(__unix__) || defined(__APPLE__)
static LlvmBackendStatus link_native_object(
    const char *object_path,
    const char *output_path,
    const LlvmBackendLinkOptions *link_options
) {
    int library_dir_count = link_options ? link_options->library_dir_count : 0;
    int library_count = link_options ? link_options->library_count : 0;
    size_t argument_count = 4u +
        (size_t)library_dir_count * 2u +
        (size_t)library_count * 2u;
    char **argv = calloc(argument_count + 1u, sizeof(*argv));
    if (!argv) {
        fprintf(stderr, "LLVM backend error: could not allocate native linker arguments\n");
        return LLVM_BACKEND_STATUS_TOOLCHAIN_ERROR;
    }

    size_t arg = 0;
    argv[arg++] = "cc";
    argv[arg++] = (char *)object_path;
    argv[arg++] = "-o";
    argv[arg++] = (char *)output_path;
    for (int i = 0; i < library_dir_count; ++i) {
        argv[arg++] = "-L";
        argv[arg++] = (char *)link_options->library_dirs[i];
    }
    for (int i = 0; i < library_count; ++i) {
        argv[arg++] = "-l";
        argv[arg++] = (char *)link_options->libraries[i];
    }
    argv[arg] = NULL;

    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "LLVM backend error: could not start native linker driver: %s\n", strerror(errno));
        free(argv);
        return LLVM_BACKEND_STATUS_TOOLCHAIN_ERROR;
    }
    if (child == 0) {
        execvp("cc", argv);
        fprintf(stderr, "LLVM backend error: could not execute native linker driver 'cc': %s\n", strerror(errno));
        _exit(127);
    }

    int wait_status = 0;
    if (waitpid(child, &wait_status, 0) < 0) {
        fprintf(stderr, "LLVM backend error: failed waiting for native linker driver: %s\n", strerror(errno));
        free(argv);
        return LLVM_BACKEND_STATUS_TOOLCHAIN_ERROR;
    }

    free(argv);
    if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
        fprintf(stderr, "LLVM backend error: native linker driver failed\n");
        return LLVM_BACKEND_STATUS_TOOLCHAIN_ERROR;
    }
    return LLVM_BACKEND_STATUS_OK;
}
#endif

LlvmBackendStatus llvm_backend_build_executable(
    const char *output_path,
    const CogIrModule *module,
    const LlvmBackendLinkOptions *link_options
) {
#if defined(__unix__) || defined(__APPLE__)
    if (!output_path) {
        fprintf(stderr, "LLVM backend error: no executable output path provided\n");
        return LLVM_BACKEND_STATUS_IO_ERROR;
    }

    char object_path[] = "/tmp/coglet-llvm-object-XXXXXX";
    int fd = mkstemp(object_path);
    if (fd < 0) {
        fprintf(stderr, "LLVM backend error: could not create temporary object file: %s\n", strerror(errno));
        return LLVM_BACKEND_STATUS_IO_ERROR;
    }
    if (close(fd) != 0) {
        fprintf(stderr, "LLVM backend error: could not close temporary object file: %s\n", strerror(errno));
        unlink(object_path);
        return LLVM_BACKEND_STATUS_IO_ERROR;
    }

    LlvmBackendStatus status = llvm_backend_emit_object_file(object_path, module);
    if (status == LLVM_BACKEND_STATUS_OK)
        status = link_native_object(object_path, output_path, link_options);

    unlink(object_path);
    return status;
#else
    (void)output_path;
    (void)module;
    (void)link_options;
    fprintf(stderr, "LLVM backend error: native executable linking is not implemented on this host platform\n");
    return LLVM_BACKEND_STATUS_TOOLCHAIN_ERROR;
#endif
}
