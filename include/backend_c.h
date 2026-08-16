#ifndef COGLET_BACKEND_C_H
#define COGLET_BACKEND_C_H

#include "cog_ir.h"

typedef enum CBackendStatus {
    C_BACKEND_STATUS_OK = 0,
    C_BACKEND_STATUS_UNSUPPORTED,
    C_BACKEND_STATUS_IO_ERROR,
    C_BACKEND_STATUS_TOOLCHAIN_ERROR,
} CBackendStatus;

typedef struct CBackendLinkOptions {
    const char *const *library_dirs;
    int library_dir_count;

    const char *const *libraries;
    int library_count;
} CBackendLinkOptions;

/*
 * Emits a frozen CogIR module as a standalone native C translation unit.
 *
 * The host-C backend owns no frontend state: AST nodes, semantic symbols, and
 * frontend Type objects may all be destroyed before this function is called.
 */
CBackendStatus c_backend_emit_file(
    const char *output_path,
    const CogIrModule *module
);

/*
 * Emits a temporary C translation unit from a frozen CogIR module and invokes
 * the native `cc` driver to produce an executable. The native compiler driver
 * also resolves the C runtime/libc and explicitly requested -L/-l options.
 */
CBackendStatus c_backend_build_executable(
    const char *output_path,
    const CogIrModule *module,
    const CBackendLinkOptions *link_options
);

#endif
