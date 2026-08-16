#ifndef COGLET_BACKEND_LLVM_INTERNAL_H
#define COGLET_BACKEND_LLVM_INTERNAL_H

#include "cog_ir.h"

#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

typedef struct LlvmCAbiSignature LlvmCAbiSignature;

typedef struct LlvmFunctionState {
    LLVMValueRef function;
    LLVMValueRef *values;
    LLVMValueRef *slots;
    LLVMBasicBlockRef *blocks;
    const LlvmCAbiSignature *c_abi;
} LlvmFunctionState;

typedef struct LlvmBackend {
    const CogIrModule *ir;
    LLVMContextRef context;
    LLVMModuleRef module;
    LLVMBuilderRef builder;
    LLVMTargetMachineRef target_machine;
    LLVMTargetDataRef target_data;
    LLVMTypeRef *types;
    LLVMTypeRef *c_aggregate_inner_types;
    unsigned char *c_aggregate_is_wrapped;
    LLVMValueRef *globals;
    LLVMValueRef *functions;
    LLVMTypeRef *function_types;
    LlvmCAbiSignature **c_function_abis;
    int had_error;
} LlvmBackend;

void llvm_backend_error(LlvmBackend *backend, const char *message);

int llvm_backend_init_native_target(LlvmBackend *backend);
int llvm_backend_init_native_asm_printer(LlvmBackend *backend);
void llvm_backend_dispose_target(LlvmBackend *backend);

LLVMTypeRef llvm_lower_type(LlvmBackend *backend, CogIrTypeId id);
LLVMTypeRef llvm_lower_function_signature(LlvmBackend *backend, CogIrTypeId id);
LLVMValueRef llvm_lower_constant(LlvmBackend *backend, CogIrConstId id);

LLVMTypeRef llvm_lower_c_object_type(LlvmBackend *backend, CogIrAbiTypeId id);
LLVMTypeRef llvm_lower_repr_c_aggregate_type(LlvmBackend *backend, const CogIrType *type);
LLVMTypeRef llvm_repr_c_inner_type(LlvmBackend *backend, CogIrTypeId id);
int llvm_repr_c_is_wrapped(LlvmBackend *backend, CogIrTypeId id);
uint64_t llvm_repr_c_field_offset(
    LlvmBackend *backend,
    const CogIrType *type,
    size_t field_index
);
LLVMValueRef llvm_build_repr_c_field_gep(
    LlvmBackend *backend,
    const CogIrType *type,
    LLVMValueRef base,
    size_t field_index
);
LLVMValueRef llvm_c_runtime_to_object(
    LlvmBackend *backend, CogIrAbiTypeId abi_type, LLVMValueRef runtime_value
);
LLVMValueRef llvm_c_object_to_runtime(
    LlvmBackend *backend, CogIrAbiTypeId abi_type, LLVMValueRef storage_value
);

LlvmCAbiSignature *llvm_build_c_function_abi(
    LlvmBackend *backend,
    const CogIrFunction *function
);
LlvmCAbiSignature *llvm_build_c_function_pointer_abi(
    LlvmBackend *backend,
    CogIrAbiTypeId abi_type
);
void llvm_dispose_c_function_abi(LlvmCAbiSignature *signature);
LLVMTypeRef llvm_c_function_abi_type(const LlvmCAbiSignature *signature);
int llvm_apply_c_function_abi(
    LlvmBackend *backend,
    LLVMValueRef function,
    const LlvmCAbiSignature *signature
);
int llvm_apply_c_call_abi(
    LlvmBackend *backend,
    LLVMValueRef call,
    const LlvmCAbiSignature *signature
);
int llvm_map_c_function_parameters(
    LlvmBackend *backend,
    const CogIrFunction *function,
    LlvmFunctionState *state,
    const LlvmCAbiSignature *signature
);
int llvm_lower_c_call(
    LlvmBackend *backend,
    const CogIrFunction *function,
    LlvmFunctionState *state,
    const CogIrInstruction *instruction,
    LLVMValueRef callee,
    const LlvmCAbiSignature *signature,
    LLVMValueRef *out_result
);
int llvm_lower_c_return(
    LlvmBackend *backend,
    LlvmFunctionState *state,
    int has_value,
    LLVMValueRef value
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
