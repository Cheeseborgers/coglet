#include "backend_llvm_internal.h"

#include <stdlib.h>
#include <string.h>

static int runtime_is_aggregate(const CogIrType *type)
{
    return type && (type->kind == COG_IR_TYPE_STRUCT || type->kind == COG_IR_TYPE_UNION ||
                    type->kind == COG_IR_TYPE_ARRAY);
}

static int abi_type_contains_c_bool_object_pointer(
    LlvmBackend *backend,
    CogIrAbiTypeId id
) {
    const CogIrAbiType *abi = cog_ir_get_abi_type(backend->ir, id);
    if (!abi)
        return 0;

    switch (abi->kind) {
        case COG_IR_ABI_TYPE_POINTER: {
            const CogIrAbiType *element = cog_ir_get_abi_type(
                backend->ir, abi->element_type);
            if (!element)
                return 0;
            if (element->kind == COG_IR_ABI_TYPE_C_SCALAR &&
                element->c_scalar_kind == COG_IR_C_SCALAR_BOOL)
                return 1;
            return abi_type_contains_c_bool_object_pointer(
                backend, abi->element_type);
        }
        case COG_IR_ABI_TYPE_ARRAY:
            return abi_type_contains_c_bool_object_pointer(
                backend, abi->element_type);
        case COG_IR_ABI_TYPE_FUNCTION:
            if (abi_type_contains_c_bool_object_pointer(
                    backend, abi->return_type))
                return 1;
            for (size_t i = 0; i < abi->parameter_count; ++i) {
                if (abi_type_contains_c_bool_object_pointer(
                        backend, abi->parameter_types[i]))
                    return 1;
            }
            return 0;
        default:
            return 0;
    }
}

static LLVMTypeRef lower_c_abi_value_type(LlvmBackend *backend, CogIrAbiTypeId id)
{
    const CogIrAbiType *abi = cog_ir_get_abi_type(backend->ir, id);
    if (!abi) {
        llvm_backend_error(backend, "invalid CogIR C ABI type");
        return NULL;
    }

    const CogIrType *runtime = cog_ir_get_type(backend->ir, abi->runtime_type);
    if (!runtime) {
        llvm_backend_error(backend, "C ABI type references an invalid runtime type");
        return NULL;
    }

    switch (abi->kind) {
        case COG_IR_ABI_TYPE_SEMANTIC:
        case COG_IR_ABI_TYPE_C_SCALAR:
            if (runtime_is_aggregate(runtime)) {
                llvm_backend_error(
                    backend,
                    "#repr(c) aggregate argument/return classification is outside the LLVM Stage 5 scalar ABI subset"
                );
                return NULL;
            }
            return llvm_lower_type(backend, abi->runtime_type);

        case COG_IR_ABI_TYPE_POINTER:
            if (abi_type_contains_c_bool_object_pointer(backend, id)) {
                llvm_backend_error(
                    backend,
                    "C _Bool object storage through typed pointers is outside the LLVM Stage 5 scalar ABI subset"
                );
                return NULL;
            }
            return LLVMPointerTypeInContext(backend->context, 0);

        case COG_IR_ABI_TYPE_OPAQUE_POINTER:
        case COG_IR_ABI_TYPE_FUNCTION:
            return LLVMPointerTypeInContext(backend->context, 0);

        case COG_IR_ABI_TYPE_ARRAY:
            llvm_backend_error(
                backend,
                "C array ABI values require represented aggregate/layout lowering outside the LLVM Stage 5 scalar ABI subset"
            );
            return NULL;
    }

    llvm_backend_error(backend, "unsupported CogIR C ABI type kind");
    return NULL;
}

static LLVMTypeRef build_c_function_type(
    LlvmBackend *backend,
    const CogIrType *runtime,
    CogIrAbiTypeId return_abi_type,
    const CogIrAbiTypeId *parameter_abi_types,
    size_t parameter_count
) {
    if (!runtime || runtime->kind != COG_IR_TYPE_FUNCTION || runtime->as.function.abi != COG_IR_ABI_C) {
        llvm_backend_error(backend, "C ABI signature references a non-C function type");
        return NULL;
    }
    if (runtime->as.function.parameter_count != parameter_count) {
        llvm_backend_error(backend, "C ABI parameter metadata does not match runtime function type");
        return NULL;
    }

    LLVMTypeRef result = lower_c_abi_value_type(backend, return_abi_type);
    if (!result)
        return NULL;

    LLVMTypeRef *params = NULL;
    if (parameter_count) {
        params = calloc(parameter_count, sizeof(*params));
        if (!params) {
            llvm_backend_error(backend, "out of memory lowering C ABI function signature");
            return NULL;
        }
        for (size_t i = 0; i < parameter_count; ++i) {
            params[i] = lower_c_abi_value_type(backend, parameter_abi_types[i]);
            if (!params[i]) {
                free(params);
                return NULL;
            }
        }
    }

    LLVMTypeRef result_type = LLVMFunctionType(
        result,
        params,
        (unsigned)parameter_count,
        runtime->as.function.is_variadic ? 1 : 0
    );
    free(params);
    return result_type;
}

LLVMTypeRef llvm_lower_c_function_signature(LlvmBackend *backend, const CogIrFunction *function)
{
    if (!function || function->abi.abi != COG_IR_ABI_C) {
        llvm_backend_error(backend, "requested C ABI lowering for a non-C function");
        return NULL;
    }
    const CogIrType *runtime = cog_ir_get_type(backend->ir, function->type);
    return build_c_function_type(
        backend,
        runtime,
        function->abi.return_abi_type,
        function->abi.parameter_abi_types,
        function->abi.parameter_count
    );
}

LLVMTypeRef llvm_lower_c_function_pointer_signature(LlvmBackend *backend, CogIrAbiTypeId abi_type)
{
    const CogIrAbiType *abi = cog_ir_get_abi_type(backend->ir, abi_type);
    if (!abi || abi->kind != COG_IR_ABI_TYPE_FUNCTION) {
        llvm_backend_error(backend, "C call is missing exact function-pointer ABI metadata");
        return NULL;
    }
    const CogIrType *runtime = cog_ir_get_type(backend->ir, abi->runtime_type);
    return build_c_function_type(
        backend,
        runtime,
        abi->return_type,
        abi->parameter_types,
        abi->parameter_count
    );
}

static int target_is_x86_family(LlvmBackend *backend, int *is_x86_64, int *is_windows)
{
    const char *triple = LLVMGetTarget(backend->module);
    if (!triple)
        triple = "";
    int x64 = strncmp(triple, "x86_64", 6) == 0 || strncmp(triple, "amd64", 5) == 0;
    int x86 = x64 || strncmp(triple, "i386", 4) == 0 || strncmp(triple, "i486", 4) == 0 ||
              strncmp(triple, "i586", 4) == 0 || strncmp(triple, "i686", 4) == 0;
    int windows = strstr(triple, "windows") != NULL || strstr(triple, "win32") != NULL ||
                  strstr(triple, "mingw") != NULL;
    if (is_x86_64) *is_x86_64 = x64;
    if (is_windows) *is_windows = windows;
    return x86;
}

static int lower_calling_convention(
    LlvmBackend *backend,
    CogIrCallingConvention convention,
    unsigned *out
) {
    int is_x86_64 = 0;
    int is_windows = 0;
    int is_x86 = target_is_x86_family(backend, &is_x86_64, &is_windows);
    (void)is_windows;

    switch (convention) {
        case COG_IR_CALL_DEFAULT:
        case COG_IR_CALL_CDECL:
            *out = LLVMCCallConv;
            return 1;
        case COG_IR_CALL_STDCALL:
            if (!is_x86) {
                llvm_backend_error(backend, "call=stdcall requires an x86 LLVM target");
                return 0;
            }
            *out = LLVMX86StdcallCallConv;
            return 1;
        case COG_IR_CALL_SYSV64:
            if (!is_x86_64) {
                llvm_backend_error(backend, "call=sysv64 requires an x86-64 LLVM target");
                return 0;
            }
            *out = LLVMX8664SysVCallConv;
            return 1;
        case COG_IR_CALL_WIN64:
            if (!is_x86_64) {
                llvm_backend_error(backend, "call=win64 requires an x86-64 LLVM target");
                return 0;
            }
            *out = LLVMWin64CallConv;
            return 1;
    }

    llvm_backend_error(backend, "unknown CogIR C calling convention");
    return 0;
}

static int uses_x86_integer_extensions(
    LlvmBackend *backend,
    CogIrCallingConvention convention
) {
    int is_x86_64 = 0;
    int is_windows = 0;
    int is_x86 = target_is_x86_family(backend, &is_x86_64, &is_windows);
    if (!is_x86)
        return 0;
    if (!is_x86_64)
        return 1;
    if (convention == COG_IR_CALL_SYSV64)
        return 1;
    if (convention == COG_IR_CALL_WIN64)
        return 0;
    return !is_windows;
}

static const char *extension_attribute_for_abi_type(
    LlvmBackend *backend,
    CogIrCallingConvention convention,
    CogIrAbiTypeId id
) {
    if (!uses_x86_integer_extensions(backend, convention))
        return NULL;

    const CogIrAbiType *abi = cog_ir_get_abi_type(backend->ir, id);
    if (!abi || abi->kind != COG_IR_ABI_TYPE_C_SCALAR)
        return NULL;

    switch (abi->c_scalar_kind) {
        case COG_IR_C_SCALAR_BOOL:
        case COG_IR_C_SCALAR_UCHAR:
        case COG_IR_C_SCALAR_USHORT:
            return "zeroext";
        case COG_IR_C_SCALAR_SCHAR:
        case COG_IR_C_SCALAR_SHORT:
            return "signext";
        case COG_IR_C_SCALAR_CHAR:
            return backend->ir->target.c_char_is_signed ? "signext" : "zeroext";
        default:
            return NULL;
    }
}

static LLVMAttributeRef make_enum_attribute(LlvmBackend *backend, const char *name)
{
    unsigned kind = LLVMGetEnumAttributeKindForName(name, strlen(name));
    if (!kind) {
        llvm_backend_error(backend, "LLVM does not expose a required C ABI extension attribute");
        return NULL;
    }
    return LLVMCreateEnumAttribute(backend->context, kind, 0);
}

int llvm_apply_c_function_abi(
    LlvmBackend *backend,
    LLVMValueRef llvm_function,
    CogIrCallingConvention calling_convention,
    CogIrAbiTypeId return_abi_type,
    const CogIrAbiTypeId *parameter_abi_types,
    size_t parameter_count
) {
    unsigned llvm_cc = 0;
    if (!lower_calling_convention(backend, calling_convention, &llvm_cc))
        return 0;
    LLVMSetFunctionCallConv(llvm_function, llvm_cc);

    const char *ret_attr = extension_attribute_for_abi_type(backend, calling_convention, return_abi_type);
    if (ret_attr) {
        LLVMAttributeRef attr = make_enum_attribute(backend, ret_attr);
        if (!attr) return 0;
        LLVMAddAttributeAtIndex(llvm_function, LLVMAttributeReturnIndex, attr);
    }

    for (size_t i = 0; i < parameter_count; ++i) {
        const char *name = extension_attribute_for_abi_type(backend, calling_convention, parameter_abi_types[i]);
        if (!name) continue;
        LLVMAttributeRef attr = make_enum_attribute(backend, name);
        if (!attr) return 0;
        LLVMAddAttributeAtIndex(llvm_function, (LLVMAttributeIndex)(i + 1), attr);
    }
    return 1;
}

int llvm_apply_c_call_abi(
    LlvmBackend *backend,
    LLVMValueRef call,
    CogIrCallingConvention calling_convention,
    const CogIrAbiType *function_abi
) {
    if (!function_abi || function_abi->kind != COG_IR_ABI_TYPE_FUNCTION) {
        llvm_backend_error(backend, "C call is missing function ABI metadata");
        return 0;
    }
    unsigned llvm_cc = 0;
    if (!lower_calling_convention(backend, calling_convention, &llvm_cc))
        return 0;
    LLVMSetInstructionCallConv(call, llvm_cc);

    const char *ret_attr = extension_attribute_for_abi_type(
        backend, calling_convention, function_abi->return_type);
    if (ret_attr) {
        LLVMAttributeRef attr = make_enum_attribute(backend, ret_attr);
        if (!attr) return 0;
        LLVMAddCallSiteAttribute(call, LLVMAttributeReturnIndex, attr);
    }
    for (size_t i = 0; i < function_abi->parameter_count; ++i) {
        const char *name = extension_attribute_for_abi_type(
            backend, calling_convention, function_abi->parameter_types[i]);
        if (!name) continue;
        LLVMAttributeRef attr = make_enum_attribute(backend, name);
        if (!attr) return 0;
        LLVMAddCallSiteAttribute(call, (LLVMAttributeIndex)(i + 1), attr);
    }
    return 1;
}

static const CogIrType *integer_like_type(const CogIrModule *module, CogIrTypeId id)
{
    const CogIrType *type = cog_ir_get_type(module, id);
    if (type && type->kind == COG_IR_TYPE_ENUM)
        type = cog_ir_get_type(module, type->as.enumeration.backing_type);
    return type;
}

int llvm_lower_c_vararg_promotion(
    LlvmBackend *backend,
    const CogIrFunction *function,
    LlvmFunctionState *state,
    const CogIrInstruction *instruction,
    LLVMValueRef *out_result
) {
    const CogIrValue *operand = cog_ir_get_value(function, instruction->as.conversion.operand);
    const CogIrType *source = operand ? integer_like_type(backend->ir, operand->type) : NULL;
    const CogIrType *target = integer_like_type(backend->ir, instruction->result_type);
    LLVMValueRef value = operand ? state->values[operand->id] : NULL;
    if (!source || !target || !value) {
        llvm_backend_error(backend, "invalid c.vararg.promote operands");
        return 0;
    }

    if (source->kind == COG_IR_TYPE_BOOL && target->kind == COG_IR_TYPE_INTEGER) {
        *out_result = LLVMBuildZExt(backend->builder, value, llvm_lower_type(backend, instruction->result_type), "");
        return *out_result != NULL;
    }
    if (source->kind == COG_IR_TYPE_INTEGER && target->kind == COG_IR_TYPE_INTEGER) {
        LLVMTypeRef target_type = llvm_lower_type(backend, instruction->result_type);
        if (!target_type) return 0;
        if (source->as.integer.bits == target->as.integer.bits) {
            *out_result = value;
        } else if (source->as.integer.bits < target->as.integer.bits) {
            *out_result = source->as.integer.is_signed
                ? LLVMBuildSExt(backend->builder, value, target_type, "")
                : LLVMBuildZExt(backend->builder, value, target_type, "");
        } else {
            llvm_backend_error(backend, "C default integer promotion unexpectedly narrows its operand");
            return 0;
        }
        return *out_result != NULL;
    }
    if (source->kind == COG_IR_TYPE_FLOAT && source->as.floating.bits == 32 &&
        target->kind == COG_IR_TYPE_FLOAT && target->as.floating.bits == 64) {
        LLVMTypeRef target_type = llvm_lower_type(backend, instruction->result_type);
        if (!target_type) return 0;
        *out_result = LLVMBuildFPExt(backend->builder, value, target_type, "");
        return *out_result != NULL;
    }

    llvm_backend_error(backend, "unsupported C default argument promotion reached LLVM lowering");
    return 0;
}
