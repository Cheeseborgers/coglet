#ifndef COGLET_BACKEND_LLVM_INTERNAL_H
#define COGLET_BACKEND_LLVM_INTERNAL_H

#include "cog_ir.h"

#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

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
    LLVMTargetMachineRef target_machine;
    LLVMTargetDataRef target_data;
    LLVMTypeRef *types;
    LLVMValueRef *globals;
    LLVMValueRef *functions;
    LLVMTypeRef *function_types;
    int had_error;
} LlvmBackend;

void llvm_backend_error(LlvmBackend *backend, const char *message);

int llvm_backend_init_native_target(LlvmBackend *backend);
void llvm_backend_dispose_target(LlvmBackend *backend);

LLVMTypeRef llvm_lower_type(LlvmBackend *backend, CogIrTypeId id);
LLVMValueRef llvm_lower_constant(LlvmBackend *backend, CogIrConstId id);

int llvm_lower_integer_instruction(
    LlvmBackend *backend,
    const CogIrFunction *function,
    LlvmFunctionState *state,
    const CogIrInstruction *instruction,
    LLVMValueRef *out_result
);

int llvm_lower_memory_instruction(
    LlvmBackend *backend,
    const CogIrFunction *function,
    LlvmFunctionState *state,
    const CogIrInstruction *instruction,
    LLVMValueRef *out_result
);

#endif
