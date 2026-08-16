#ifndef COGLET_BACKENDS_LLVM_BACKEND_LLVM_H
#define COGLET_BACKENDS_LLVM_BACKEND_LLVM_H

#include "cog_ir.h"
#include "optimization.h"

typedef enum LlvmBackendStatus {
    LLVM_BACKEND_STATUS_OK = 0,
    LLVM_BACKEND_STATUS_UNSUPPORTED,
    LLVM_BACKEND_STATUS_INVALID_IR,
    LLVM_BACKEND_STATUS_IO_ERROR,
    LLVM_BACKEND_STATUS_CODEGEN_ERROR,
    LLVM_BACKEND_STATUS_OPTIMIZATION_ERROR,
    LLVM_BACKEND_STATUS_TOOLCHAIN_ERROR,
} LlvmBackendStatus;

typedef struct LlvmBackendOptions {
    CogOptimizationLevel optimization_level;
} LlvmBackendOptions;

typedef struct LlvmBackendLinkOptions {
    const char *const *library_dirs;
    int library_dir_count;

    const char *const *libraries;
    int library_count;
} LlvmBackendLinkOptions;

/*
 * Lowers a verified, frozen CogIR module to textual LLVM IR, verifies the
 * resulting LLVM module with LLVM's verifier, then writes it to output_path.
 *
 * The backend depends only on CogIR-owned data. Frontend AST/semantic objects
 * may already have been destroyed when this function is called.
 */
LlvmBackendStatus llvm_backend_emit_ir_file(
    const char *output_path,
    const CogIrModule *module,
    const LlvmBackendOptions *options
);

/*
 * Lowers and verifies the same frozen CogIR module, then asks LLVM's native
 * TargetMachine to emit a target object file. No external compiler process is
 * involved in object generation.
 */
LlvmBackendStatus llvm_backend_emit_object_file(
    const char *output_path,
    const CogIrModule *module,
    const LlvmBackendOptions *options
);

/*
 * Emits a temporary native object through LLVM and invokes the host linker
 * driver to produce an executable. Linker/toolchain integration is deliberately
 * separate from LLVM IR lowering and consumes only the emitted object plus the
 * explicitly requested native -L/-l inputs.
 */
LlvmBackendStatus llvm_backend_build_executable(
    const char *output_path,
    const CogIrModule *module,
    const LlvmBackendOptions *backend_options,
    const LlvmBackendLinkOptions *link_options
);

#endif
