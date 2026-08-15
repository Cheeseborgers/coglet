#ifndef COGLET_BACKEND_C_H
#define COGLET_BACKEND_C_H

#include "ast.h"
#include "semantic_anal.h"

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
 * Emits the current host-C backend subset as a standalone C translation unit.
 *
 * The frontend must already have parsed and semantically checked program.
 * Diagnostics are written to stderr. This first backend slice intentionally
 * rejects Coglet constructs whose runtime semantics are not lowered yet.
 */
CBackendStatus c_backend_emit_file(
    const char *output_path,
    const char *source_filename,
    Node *program,
    SemanticContext *sem
);

/*
 * Emits a temporary C translation unit and invokes the native `cc` driver to
 * produce an executable. The native compiler driver is also responsible for
 * resolving ordinary C runtime/libc symbols and any explicitly requested
 * library search paths / libraries.
 */
CBackendStatus c_backend_build_executable(
    const char *output_path,
    const char *source_filename,
    Node *program,
    SemanticContext *sem,
    const CBackendLinkOptions *link_options
);

#endif
