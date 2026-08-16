#ifndef COGLET_BACKEND_LLVM_INTERNAL_H
#define COGLET_BACKEND_LLVM_INTERNAL_H

#include "cog_ir.h"

#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

typedef struct LlvmFunctionState {
    LLVMValueRef function;
    LLVMValueRef *values;
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
LLVMTypeRef llvm_lower_function_signature(LlvmBackend *backend, CogIrTypeId id);
LLVMValueRef llvm_lower_constant(LlvmBackend *backend, CogIrConstId id);

LLVMTypeRef llvm_lower_c_function_signature(
    LlvmBackend *backend,
    const CogIrFunction *function
);
LLVMTypeRef llvm_lower_c_function_pointer_signature(
    LlvmBackend *backend,
    CogIrAbiTypeId abi_type
);
int llvm_apply_c_function_abi(
    LlvmBackend *backend,
    LLVMValueRef llvm_function,
    CogIrCallingConvention calling_convention,
    CogIrAbiTypeId return_abi_type,
    const CogIrAbiTypeId *parameter_abi_types,
    size_t parameter_count
);
int llvm_apply_c_call_abi(
    LlvmBackend *backend,
    LLVMValueRef call,
    CogIrCallingConvention calling_convention,
    const CogIrAbiType *function_abi
);
int llvm_lower_c_vararg_promotion(
    LlvmBackend *backend,
    const CogIrFunction *function,
    LlvmFunctionState *state,
    const CogIrInstruction *instruction,
    LLVMValueRef *out_result
);

int llvm_get_intrinsic(
    LlvmBackend *backend,
    const char *name,
    LLVMTypeRef *overload_types,
    size_t overload_count,
    LLVMValueRef *out_function,
    LLVMTypeRef *out_function_type
);
int llvm_emit_trap(LlvmBackend *backend);
int llvm_emit_trap_if(
    LlvmBackend *backend,
    LlvmFunctionState *state,
    LLVMValueRef condition
);
int llvm_lower_terminator(
    LlvmBackend *backend,
    const CogIrFunction *function,
    LlvmFunctionState *state,
    const CogIrBlock *block
);

int llvm_lower_integer_instruction(
    LlvmBackend *backend,
    const CogIrFunction *function,
    LlvmFunctionState *state,
    const CogIrInstruction *instruction,
    LLVMValueRef *out_result
);

int llvm_lower_float_instruction(
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
