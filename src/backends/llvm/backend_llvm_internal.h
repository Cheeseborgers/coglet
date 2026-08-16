#ifndef COGLET_BACKEND_LLVM_INTERNAL_H
#define COGLET_BACKEND_LLVM_INTERNAL_H

#include "cog_ir.h"

#include <llvm-c/Core.h>

typedef struct LlvmFunctionState {
    LLVMValueRef function;
    LLVMValueRef *values;
    LLVMTypeRef *callable_types;
    LLVMValueRef *slots;
    LLVMBasicBlockRef *blocks;
} LlvmFunctionState;

typedef struct LlvmBackend {
    const CogIrModule *ir;
    LLVMContextRef context;
    LLVMModuleRef module;
    LLVMBuilderRef builder;
    LLVMTypeRef *types;
    LLVMValueRef *globals;
    LLVMValueRef *functions;
    LLVMTypeRef *function_types;
    int had_error;
} LlvmBackend;

void llvm_backend_error(LlvmBackend *backend, const char *message);

int llvm_lower_integer_instruction(
    LlvmBackend *backend,
    const CogIrFunction *function,
    LlvmFunctionState *state,
    const CogIrInstruction *instruction,
    LLVMValueRef *out_result
);

#endif
