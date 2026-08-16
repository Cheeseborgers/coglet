#ifndef COGLET_BACKENDS_LLVM_BACKEND_LLVM_H
#define COGLET_BACKENDS_LLVM_BACKEND_LLVM_H

#include "cog_ir.h"

typedef enum LlvmBackendStatus {
    LLVM_BACKEND_STATUS_OK = 0,
    LLVM_BACKEND_STATUS_UNSUPPORTED,
    LLVM_BACKEND_STATUS_INVALID_IR,
    LLVM_BACKEND_STATUS_IO_ERROR,
} LlvmBackendStatus;

/*
 * Lowers a verified, frozen CogIR module to textual LLVM IR, verifies the
 * resulting LLVM module with LLVM's verifier, then writes it to output_path.
 *
 * The backend depends only on CogIR-owned data. Frontend AST/semantic objects
 * may already have been destroyed when this function is called.
 */
LlvmBackendStatus llvm_backend_emit_ir_file(
    const char *output_path,
    const CogIrModule *module
);

#endif
