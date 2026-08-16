#include "backend_llvm_internal.h"

#include <stdlib.h>
#include <string.h>

typedef enum LlvmCAbiPassKind {
    LLVM_C_ABI_DIRECT,
    LLVM_C_ABI_COERCE,
    LLVM_C_ABI_INDIRECT,
} LlvmCAbiPassKind;

typedef struct LlvmCAbiValuePlan {
    CogIrAbiTypeId abi_type;
    CogIrTypeId runtime_type;
    LlvmCAbiPassKind kind;
    LLVMTypeRef storage_type;
    LLVMTypeRef components[2];
    unsigned component_count;
    unsigned alignment;
    unsigned llvm_parameter_start;
    int is_aggregate;
    int is_sret;
    int is_byval;
} LlvmCAbiValuePlan;

struct LlvmCAbiSignature {
    LLVMTypeRef function_type;
    LlvmCAbiValuePlan result;
    LlvmCAbiValuePlan *parameters;
    size_t parameter_count;
    unsigned llvm_parameter_count;
    CogIrCallingConvention calling_convention;
    int is_variadic;
};

typedef enum SysVClass {
    SYSV_NO_CLASS,
    SYSV_INTEGER,
    SYSV_SSE,
    SYSV_MEMORY,
} SysVClass;

typedef struct SysVClassification {
    SysVClass classes[2];
    unsigned used_bytes[2];
    int memory;
} SysVClassification;

static int runtime_is_aggregate(const CogIrType *type)
{
    return type && (type->kind == COG_IR_TYPE_STRUCT || type->kind == COG_IR_TYPE_UNION ||
                    type->kind == COG_IR_TYPE_ARRAY);
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

static LLVMAttributeRef make_enum_attribute(
    LlvmBackend *backend,
    const char *name,
    uint64_t value
) {
    unsigned kind = LLVMGetEnumAttributeKindForName(name, strlen(name));
    if (!kind) {
        llvm_backend_error(backend, "LLVM does not expose a required C ABI attribute");
        return NULL;
    }
    return LLVMCreateEnumAttribute(backend->context, kind, value);
}

static LLVMAttributeRef make_type_attribute(
    LlvmBackend *backend,
    const char *name,
    LLVMTypeRef type
) {
    unsigned kind = LLVMGetEnumAttributeKindForName(name, strlen(name));
    if (!kind) {
        llvm_backend_error(backend, "LLVM does not expose a required C ABI type attribute");
        return NULL;
    }
    return LLVMCreateTypeAttribute(backend->context, kind, type);
}

static int abi_object_requires_distinct_storage(
    LlvmBackend *backend,
    CogIrAbiTypeId id
) {
    const CogIrAbiType *abi = cog_ir_get_abi_type(backend->ir, id);
    if (!abi)
        return 0;
    switch (abi->kind) {
        case COG_IR_ABI_TYPE_C_SCALAR:
            return abi->c_scalar_kind == COG_IR_C_SCALAR_BOOL;
        case COG_IR_ABI_TYPE_ARRAY:
            return abi_object_requires_distinct_storage(backend, abi->element_type);
        case COG_IR_ABI_TYPE_POINTER:
        case COG_IR_ABI_TYPE_SEMANTIC:
        case COG_IR_ABI_TYPE_OPAQUE_POINTER:
        case COG_IR_ABI_TYPE_FUNCTION:
            return 0;
    }
    return 0;
}

static int abi_pointer_requires_distinct_object_storage(
    LlvmBackend *backend,
    CogIrAbiTypeId id
) {
    const CogIrAbiType *abi = cog_ir_get_abi_type(backend->ir, id);
    return abi && abi->kind == COG_IR_ABI_TYPE_POINTER &&
           abi_object_requires_distinct_storage(backend, abi->element_type);
}

static int abi_storage_spelling_compatible(
    LlvmBackend *backend,
    CogIrAbiTypeId expected_id,
    CogIrAbiTypeId actual_id
) {
    const CogIrAbiType *expected = cog_ir_get_abi_type(backend->ir, expected_id);
    const CogIrAbiType *actual = cog_ir_get_abi_type(backend->ir, actual_id);
    if (!expected || !actual)
        return 0;
    if (expected->kind == COG_IR_ABI_TYPE_C_SCALAR &&
        expected->c_scalar_kind == COG_IR_C_SCALAR_BOOL) {
        return actual->kind == COG_IR_ABI_TYPE_C_SCALAR &&
               actual->c_scalar_kind == COG_IR_C_SCALAR_BOOL;
    }
    if ((expected->kind == COG_IR_ABI_TYPE_POINTER || expected->kind == COG_IR_ABI_TYPE_ARRAY) &&
        actual->kind == expected->kind) {
        return abi_storage_spelling_compatible(
            backend, expected->element_type, actual->element_type);
    }
    return 1;
}

static LLVMTypeRef lower_scalar_c_abi_value_type(
    LlvmBackend *backend,
    CogIrAbiTypeId id
) {
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
            if (runtime_is_aggregate(runtime))
                return NULL;
            return llvm_lower_type(backend, abi->runtime_type);
        case COG_IR_ABI_TYPE_POINTER:
        case COG_IR_ABI_TYPE_OPAQUE_POINTER:
        case COG_IR_ABI_TYPE_FUNCTION:
            return LLVMPointerTypeInContext(backend->context, 0);
        case COG_IR_ABI_TYPE_ARRAY:
            return NULL;
    }
    return NULL;
}

static SysVClass merge_sysv_class(SysVClass a, SysVClass b)
{
    if (a == b)
        return a;
    if (a == SYSV_NO_CLASS)
        return b;
    if (b == SYSV_NO_CLASS)
        return a;
    if (a == SYSV_MEMORY || b == SYSV_MEMORY)
        return SYSV_MEMORY;
    if (a == SYSV_INTEGER || b == SYSV_INTEGER)
        return SYSV_INTEGER;
    return SYSV_SSE;
}

static void sysv_mark_range(
    SysVClassification *out,
    uint64_t offset,
    uint64_t size,
    SysVClass cls
) {
    if (out->memory || !size)
        return;
    uint64_t end = offset + size;
    if (end > 16) {
        out->memory = 1;
        return;
    }
    for (unsigned chunk = (unsigned)(offset / 8); chunk <= (unsigned)((end - 1) / 8); ++chunk) {
        if (chunk >= 2) {
            out->memory = 1;
            return;
        }
        out->classes[chunk] = merge_sysv_class(out->classes[chunk], cls);
        if (out->classes[chunk] == SYSV_MEMORY) {
            out->memory = 1;
            return;
        }
        uint64_t chunk_start = (uint64_t)chunk * 8;
        uint64_t used_end = end < chunk_start + 8 ? end : chunk_start + 8;
        unsigned used = (unsigned)(used_end - chunk_start);
        if (used > out->used_bytes[chunk])
            out->used_bytes[chunk] = used;
    }
}

static int classify_sysv_object(
    LlvmBackend *backend,
    CogIrAbiTypeId abi_id,
    uint64_t base_offset,
    SysVClassification *out
);

static int classify_sysv_runtime_aggregate(
    LlvmBackend *backend,
    const CogIrType *type,
    uint64_t base_offset,
    SysVClassification *out
) {
    LLVMTypeRef storage = llvm_lower_type(backend, type->id);
    if (!storage)
        return 0;
    if (LLVMABISizeOfType(backend->target_data, storage) > 16) {
        out->memory = 1;
        return 1;
    }

    if (type->kind == COG_IR_TYPE_STRUCT) {
        for (size_t i = 0; i < type->as.aggregate.field_count; ++i) {
            CogIrAbiTypeId field_abi = type->as.aggregate.fields[i].abi_type;
            LLVMTypeRef field_storage = llvm_lower_c_object_type(backend, field_abi);
            uint64_t field_offset = llvm_repr_c_field_offset(backend, type, i);
            if (!field_storage || field_offset == UINT64_MAX)
                return 0;
            unsigned field_align = LLVMABIAlignmentOfType(backend->target_data, field_storage);
            uint64_t absolute = base_offset + field_offset;
            if (field_align && absolute % field_align != 0) {
                out->memory = 1;
                return 1;
            }
            if (!classify_sysv_object(backend, field_abi, absolute, out))
                return 0;
            if (out->memory)
                return 1;
        }
        return 1;
    }

    if (type->kind == COG_IR_TYPE_UNION) {
        for (size_t i = 0; i < type->as.aggregate.field_count; ++i) {
            CogIrAbiTypeId field_abi = type->as.aggregate.fields[i].abi_type;
            LLVMTypeRef field_storage = llvm_lower_c_object_type(backend, field_abi);
            if (!field_storage)
                return 0;
            unsigned field_align = LLVMABIAlignmentOfType(backend->target_data, field_storage);
            if (field_align && base_offset % field_align != 0) {
                out->memory = 1;
                return 1;
            }
            if (!classify_sysv_object(backend, field_abi, base_offset, out))
                return 0;
            if (out->memory)
                return 1;
        }
        return 1;
    }

    return 0;
}

static int classify_sysv_object(
    LlvmBackend *backend,
    CogIrAbiTypeId abi_id,
    uint64_t base_offset,
    SysVClassification *out
) {
    const CogIrAbiType *abi = cog_ir_get_abi_type(backend->ir, abi_id);
    if (!abi)
        return 0;
    const CogIrType *runtime = cog_ir_get_type(backend->ir, abi->runtime_type);
    if (!runtime)
        return 0;

    if (abi->kind == COG_IR_ABI_TYPE_ARRAY) {
        LLVMTypeRef element_storage = llvm_lower_c_object_type(backend, abi->element_type);
        if (!element_storage || runtime->kind != COG_IR_TYPE_ARRAY)
            return 0;
        uint64_t stride = LLVMABISizeOfType(backend->target_data, element_storage);
        unsigned align = LLVMABIAlignmentOfType(backend->target_data, element_storage);
        for (size_t i = 0; i < runtime->as.array.length; ++i) {
            uint64_t offset = base_offset + stride * i;
            if (align && offset % align != 0) {
                out->memory = 1;
                return 1;
            }
            if (!classify_sysv_object(backend, abi->element_type, offset, out))
                return 0;
            if (out->memory)
                return 1;
        }
        return 1;
    }

    if (abi->kind == COG_IR_ABI_TYPE_POINTER || abi->kind == COG_IR_ABI_TYPE_OPAQUE_POINTER ||
        abi->kind == COG_IR_ABI_TYPE_FUNCTION) {
        LLVMTypeRef storage = llvm_lower_c_object_type(backend, abi_id);
        sysv_mark_range(out, base_offset, LLVMABISizeOfType(backend->target_data, storage), SYSV_INTEGER);
        return 1;
    }

    if (runtime_is_aggregate(runtime))
        return classify_sysv_runtime_aggregate(backend, runtime, base_offset, out);

    if (runtime->kind == COG_IR_TYPE_ENUM) {
        if (runtime->as.enumeration.is_repr_c)
            return classify_sysv_object(backend, runtime->as.enumeration.backing_abi_type, base_offset, out);
        LLVMTypeRef storage = llvm_lower_c_object_type(backend, abi_id);
        sysv_mark_range(out, base_offset, LLVMABISizeOfType(backend->target_data, storage), SYSV_INTEGER);
        return 1;
    }

    LLVMTypeRef storage = llvm_lower_c_object_type(backend, abi_id);
    if (!storage)
        return 0;
    uint64_t size = LLVMABISizeOfType(backend->target_data, storage);
    SysVClass cls = runtime->kind == COG_IR_TYPE_FLOAT ? SYSV_SSE : SYSV_INTEGER;
    sysv_mark_range(out, base_offset, size, cls);
    return 1;
}

static int classify_sysv_aggregate(
    LlvmBackend *backend,
    CogIrAbiTypeId abi_id,
    LlvmCAbiValuePlan *plan,
    int is_return
) {
    LLVMTypeRef storage = llvm_lower_c_object_type(backend, abi_id);
    if (!storage)
        return 0;
    uint64_t size = LLVMABISizeOfType(backend->target_data, storage);
    unsigned align = LLVMABIAlignmentOfType(backend->target_data, storage);
    plan->storage_type = storage;
    plan->alignment = align;

    SysVClassification cls = {0};
    if (size > 16) {
        cls.memory = 1;
    } else if (!classify_sysv_object(backend, abi_id, 0, &cls)) {
        return 0;
    }

    if (cls.memory) {
        plan->kind = LLVM_C_ABI_INDIRECT;
        plan->component_count = 1;
        plan->components[0] = LLVMPointerTypeInContext(backend->context, 0);
        plan->is_sret = is_return;
        plan->is_byval = !is_return;
        return 1;
    }

    plan->kind = LLVM_C_ABI_COERCE;
    for (unsigned i = 0; i < 2; ++i) {
        if (cls.classes[i] == SYSV_NO_CLASS)
            continue;
        unsigned used = cls.used_bytes[i];
        if (!used)
            continue;
        LLVMTypeRef component = NULL;
        if (cls.classes[i] == SYSV_INTEGER) {
            component = LLVMIntTypeInContext(backend->context, used * 8);
        } else if (cls.classes[i] == SYSV_SSE) {
            component = used <= 4
                ? LLVMFloatTypeInContext(backend->context)
                : LLVMDoubleTypeInContext(backend->context);
        }
        if (!component) {
            llvm_backend_error(backend, "unsupported x86-64 SysV aggregate register class");
            return 0;
        }
        plan->components[plan->component_count++] = component;
    }
    if (!plan->component_count) {
        llvm_backend_error(backend, "#repr(c) aggregate has no ABI-visible data");
        return 0;
    }
    return 1;
}

static int classify_win64_aggregate(
    LlvmBackend *backend,
    CogIrAbiTypeId abi_id,
    LlvmCAbiValuePlan *plan,
    int is_return
) {
    LLVMTypeRef storage = llvm_lower_c_object_type(backend, abi_id);
    if (!storage)
        return 0;
    uint64_t size = LLVMABISizeOfType(backend->target_data, storage);
    plan->storage_type = storage;
    plan->alignment = LLVMABIAlignmentOfType(backend->target_data, storage);

    if (size == 1 || size == 2 || size == 4 || size == 8) {
        plan->kind = LLVM_C_ABI_COERCE;
        plan->component_count = 1;
        plan->components[0] = LLVMIntTypeInContext(backend->context, (unsigned)(size * 8));
        return 1;
    }

    plan->kind = LLVM_C_ABI_INDIRECT;
    plan->component_count = 1;
    plan->components[0] = LLVMPointerTypeInContext(backend->context, 0);
    plan->is_sret = is_return;
    plan->is_byval = 0;
    return 1;
}

static int aggregate_abi_is_win64(
    LlvmBackend *backend,
    CogIrCallingConvention convention,
    int *is_win64
) {
    int x64 = 0, windows = 0;
    target_is_x86_family(backend, &x64, &windows);
    if (!x64) {
        llvm_backend_error(backend, "represented C aggregate ABI lowering currently requires x86-64");
        return 0;
    }
    if (convention == COG_IR_CALL_SYSV64) {
        *is_win64 = 0;
        return 1;
    }
    if (convention == COG_IR_CALL_WIN64) {
        *is_win64 = 1;
        return 1;
    }
    if (convention == COG_IR_CALL_DEFAULT || convention == COG_IR_CALL_CDECL) {
        *is_win64 = windows;
        return 1;
    }
    llvm_backend_error(backend, "represented C aggregates are not supported with this calling convention");
    return 0;
}

static int build_value_plan(
    LlvmBackend *backend,
    CogIrAbiTypeId abi_id,
    CogIrCallingConvention convention,
    int is_return,
    LlvmCAbiValuePlan *plan
) {
    memset(plan, 0, sizeof(*plan));
    plan->abi_type = abi_id;
    const CogIrAbiType *abi = cog_ir_get_abi_type(backend->ir, abi_id);
    if (!abi) {
        llvm_backend_error(backend, "invalid CogIR C ABI type");
        return 0;
    }
    plan->runtime_type = abi->runtime_type;
    const CogIrType *runtime = cog_ir_get_type(backend->ir, abi->runtime_type);
    if (!runtime) {
        llvm_backend_error(backend, "C ABI value plan references an invalid runtime type");
        return 0;
    }

    if (!runtime_is_aggregate(runtime)) {
        LLVMTypeRef direct = lower_scalar_c_abi_value_type(backend, abi_id);
        if (!direct) {
            llvm_backend_error(backend, "unsupported scalar C ABI value type");
            return 0;
        }
        plan->kind = LLVM_C_ABI_DIRECT;
        plan->components[0] = direct;
        plan->component_count = 1;
        return 1;
    }

    if ((runtime->kind != COG_IR_TYPE_STRUCT && runtime->kind != COG_IR_TYPE_UNION) ||
        !runtime->as.aggregate.is_repr_c || runtime->as.aggregate.is_incomplete) {
        llvm_backend_error(backend, "C ABI aggregate value must be a complete #repr(c) struct or union");
        return 0;
    }
    plan->is_aggregate = 1;
    int win64 = 0;
    if (!aggregate_abi_is_win64(backend, convention, &win64))
        return 0;
    return win64
        ? classify_win64_aggregate(backend, abi_id, plan, is_return)
        : classify_sysv_aggregate(backend, abi_id, plan, is_return);
}

static LLVMTypeRef aggregate_return_type(LlvmBackend *backend, const LlvmCAbiValuePlan *plan)
{
    if (plan->kind == LLVM_C_ABI_INDIRECT)
        return LLVMVoidTypeInContext(backend->context);
    if (plan->component_count == 1)
        return plan->components[0];
    return LLVMStructTypeInContext(
        backend->context, (LLVMTypeRef *)plan->components, plan->component_count, 0);
}

static LlvmCAbiSignature *build_signature(
    LlvmBackend *backend,
    const CogIrType *runtime,
    CogIrCallingConvention convention,
    CogIrAbiTypeId return_abi_type,
    const CogIrAbiTypeId *parameter_abi_types,
    size_t parameter_count
) {
    if (!runtime || runtime->kind != COG_IR_TYPE_FUNCTION || runtime->as.function.abi != COG_IR_ABI_C ||
        runtime->as.function.parameter_count != parameter_count) {
        llvm_backend_error(backend, "C ABI signature metadata does not match runtime function type");
        return NULL;
    }

    LlvmCAbiSignature *signature = calloc(1, sizeof(*signature));
    if (!signature) {
        llvm_backend_error(backend, "out of memory building C ABI signature");
        return NULL;
    }
    signature->calling_convention = convention;
    signature->parameter_count = parameter_count;
    signature->is_variadic = runtime->as.function.is_variadic;
    signature->parameters = parameter_count ? calloc(parameter_count, sizeof(*signature->parameters)) : NULL;
    if (parameter_count && !signature->parameters) {
        free(signature);
        llvm_backend_error(backend, "out of memory building C ABI parameter plans");
        return NULL;
    }
    if (!build_value_plan(backend, return_abi_type, convention, 1, &signature->result))
        goto fail;

    unsigned llvm_count = signature->result.is_sret ? 1u : 0u;
    for (size_t i = 0; i < parameter_count; ++i) {
        if (!build_value_plan(backend, parameter_abi_types[i], convention, 0, &signature->parameters[i]))
            goto fail;
        signature->parameters[i].llvm_parameter_start = llvm_count;
        llvm_count += signature->parameters[i].component_count;
    }
    signature->llvm_parameter_count = llvm_count;

    LLVMTypeRef *params = llvm_count ? calloc(llvm_count, sizeof(*params)) : NULL;
    if (llvm_count && !params) {
        llvm_backend_error(backend, "out of memory building lowered C function type");
        goto fail;
    }
    unsigned at = 0;
    if (signature->result.is_sret)
        params[at++] = LLVMPointerTypeInContext(backend->context, 0);
    for (size_t i = 0; i < parameter_count; ++i) {
        for (unsigned c = 0; c < signature->parameters[i].component_count; ++c)
            params[at++] = signature->parameters[i].components[c];
    }
    LLVMTypeRef result = aggregate_return_type(backend, &signature->result);
    signature->function_type = LLVMFunctionType(
        result, params, llvm_count, signature->is_variadic ? 1 : 0);
    free(params);
    return signature;

fail:
    free(signature->parameters);
    free(signature);
    return NULL;
}

LlvmCAbiSignature *llvm_build_c_function_abi(
    LlvmBackend *backend,
    const CogIrFunction *function
) {
    if (!function || function->abi.abi != COG_IR_ABI_C) {
        llvm_backend_error(backend, "requested C ABI lowering for a non-C function");
        return NULL;
    }
    const CogIrType *runtime = cog_ir_get_type(backend->ir, function->type);
    return build_signature(
        backend, runtime, function->abi.calling_convention,
        function->abi.return_abi_type, function->abi.parameter_abi_types,
        function->abi.parameter_count);
}

LlvmCAbiSignature *llvm_build_c_function_pointer_abi(
    LlvmBackend *backend,
    CogIrAbiTypeId abi_type
) {
    const CogIrAbiType *abi = cog_ir_get_abi_type(backend->ir, abi_type);
    if (!abi || abi->kind != COG_IR_ABI_TYPE_FUNCTION) {
        llvm_backend_error(backend, "C call is missing exact function-pointer ABI metadata");
        return NULL;
    }
    const CogIrType *runtime = cog_ir_get_type(backend->ir, abi->runtime_type);
    if (!runtime || runtime->kind != COG_IR_TYPE_FUNCTION) {
        llvm_backend_error(backend, "C function-pointer ABI metadata references a non-function runtime type");
        return NULL;
    }
    return build_signature(
        backend, runtime, runtime->as.function.calling_convention,
        abi->return_type, abi->parameter_types, abi->parameter_count);
}

void llvm_dispose_c_function_abi(LlvmCAbiSignature *signature)
{
    if (!signature)
        return;
    free(signature->parameters);
    free(signature);
}

LLVMTypeRef llvm_c_function_abi_type(const LlvmCAbiSignature *signature)
{
    return signature ? signature->function_type : NULL;
}

static int add_function_attribute(
    LLVMValueRef function,
    LLVMAttributeIndex index,
    LLVMAttributeRef attribute
) {
    if (!attribute)
        return 0;
    LLVMAddAttributeAtIndex(function, index, attribute);
    return 1;
}

static int add_call_attribute(
    LLVMValueRef call,
    LLVMAttributeIndex index,
    LLVMAttributeRef attribute
) {
    if (!attribute)
        return 0;
    LLVMAddCallSiteAttribute(call, index, attribute);
    return 1;
}

static int apply_value_parameter_attributes(
    LlvmBackend *backend,
    LLVMValueRef value,
    int is_call,
    const LlvmCAbiValuePlan *plan
) {
    LLVMAttributeIndex index = (LLVMAttributeIndex)(plan->llvm_parameter_start + 1);
    if (plan->is_byval) {
        LLVMAttributeRef byval = make_type_attribute(backend, "byval", plan->storage_type);
        LLVMAttributeRef align = make_enum_attribute(backend, "align", plan->alignment < 8 ? 8 : plan->alignment);
        if (is_call) {
            if (!add_call_attribute(value, index, byval) ||
                !add_call_attribute(value, index, align))
                return 0;
        } else {
            if (!add_function_attribute(value, index, byval) ||
                !add_function_attribute(value, index, align))
                return 0;
        }
    }
    return 1;
}

static int apply_signature_attributes(
    LlvmBackend *backend,
    LLVMValueRef value,
    const LlvmCAbiSignature *signature,
    int is_call
) {
    unsigned llvm_cc = 0;
    if (!lower_calling_convention(backend, signature->calling_convention, &llvm_cc))
        return 0;
    if (is_call)
        LLVMSetInstructionCallConv(value, llvm_cc);
    else
        LLVMSetFunctionCallConv(value, llvm_cc);

    if (signature->result.is_sret) {
        LLVMAttributeRef sret = make_type_attribute(backend, "sret", signature->result.storage_type);
        LLVMAttributeRef align = make_enum_attribute(backend, "align", signature->result.alignment);
        if (is_call) {
            if (!add_call_attribute(value, 1, sret) ||
                !add_call_attribute(value, 1, align))
                return 0;
        } else {
            if (!add_function_attribute(value, 1, sret) ||
                !add_function_attribute(value, 1, align))
                return 0;
        }
    } else if (!signature->result.is_aggregate) {
        const char *ret_attr = extension_attribute_for_abi_type(
            backend, signature->calling_convention, signature->result.abi_type);
        if (ret_attr) {
            LLVMAttributeRef attr = make_enum_attribute(backend, ret_attr, 0);
            if (is_call) {
                if (!add_call_attribute(value, LLVMAttributeReturnIndex, attr))
                    return 0;
            } else if (!add_function_attribute(value, LLVMAttributeReturnIndex, attr)) {
                return 0;
            }
        }
    }

    for (size_t i = 0; i < signature->parameter_count; ++i) {
        const LlvmCAbiValuePlan *plan = &signature->parameters[i];
        if (!apply_value_parameter_attributes(backend, value, is_call, plan))
            return 0;
        if (!plan->is_aggregate) {
            const char *name = extension_attribute_for_abi_type(
                backend, signature->calling_convention, plan->abi_type);
            if (name) {
                LLVMAttributeRef attr = make_enum_attribute(backend, name, 0);
                LLVMAttributeIndex index = (LLVMAttributeIndex)(plan->llvm_parameter_start + 1);
                if (is_call) {
                    if (!add_call_attribute(value, index, attr))
                        return 0;
                } else if (!add_function_attribute(value, index, attr)) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

int llvm_apply_c_function_abi(
    LlvmBackend *backend,
    LLVMValueRef function,
    const LlvmCAbiSignature *signature
) {
    return apply_signature_attributes(backend, function, signature, 0);
}

int llvm_apply_c_call_abi(
    LlvmBackend *backend,
    LLVMValueRef call,
    const LlvmCAbiSignature *signature
) {
    return apply_signature_attributes(backend, call, signature, 1);
}

static LLVMValueRef byte_offset_pointer(
    LlvmBackend *backend,
    LLVMValueRef base,
    unsigned offset
) {
    if (!offset)
        return base;
    LLVMValueRef index = LLVMConstInt(
        LLVMIntTypeInContext(backend->context, 64), offset, 0);
    return LLVMBuildGEP2(
        backend->builder,
        LLVMInt8TypeInContext(backend->context),
        base,
        &index,
        1,
        "");
}

static LLVMValueRef alloca_storage(
    LlvmBackend *backend,
    const LlvmCAbiValuePlan *plan
) {
    LLVMValueRef slot = LLVMBuildAlloca(backend->builder, plan->storage_type, "");
    if (slot && plan->alignment)
        LLVMSetAlignment(slot, plan->alignment);
    return slot;
}

static int split_aggregate_value(
    LlvmBackend *backend,
    const LlvmCAbiValuePlan *plan,
    LLVMValueRef value,
    LLVMValueRef *components
) {
    LLVMValueRef slot = alloca_storage(backend, plan);
    if (!slot)
        return 0;
    LLVMValueRef store = LLVMBuildStore(backend->builder, value, slot);
    if (!store)
        return 0;
    for (unsigned i = 0; i < plan->component_count; ++i) {
        LLVMValueRef ptr = byte_offset_pointer(backend, slot, i * 8);
        components[i] = LLVMBuildLoad2(backend->builder, plan->components[i], ptr, "");
        if (!components[i])
            return 0;
        LLVMSetAlignment(components[i], 1);
    }
    return 1;
}

static LLVMValueRef join_aggregate_components(
    LlvmBackend *backend,
    const LlvmCAbiValuePlan *plan,
    LLVMValueRef *components
) {
    LLVMValueRef slot = alloca_storage(backend, plan);
    if (!slot)
        return NULL;
    LLVMValueRef zero = LLVMConstNull(plan->storage_type);
    if (!LLVMBuildStore(backend->builder, zero, slot))
        return NULL;
    for (unsigned i = 0; i < plan->component_count; ++i) {
        LLVMValueRef ptr = byte_offset_pointer(backend, slot, i * 8);
        LLVMValueRef store = LLVMBuildStore(backend->builder, components[i], ptr);
        if (!store)
            return NULL;
        LLVMSetAlignment(store, 1);
    }
    LLVMValueRef result = LLVMBuildLoad2(backend->builder, plan->storage_type, slot, "");
    if (result)
        LLVMSetAlignment(result, plan->alignment ? plan->alignment : 1);
    return result;
}

int llvm_map_c_function_parameters(
    LlvmBackend *backend,
    const CogIrFunction *function,
    LlvmFunctionState *state,
    const LlvmCAbiSignature *signature
) {
    if (!signature || signature->parameter_count != function->parameter_count) {
        llvm_backend_error(backend, "C ABI function parameter plan does not match CogIR parameters");
        return 0;
    }
    for (size_t i = 0; i < function->parameter_count; ++i) {
        const LlvmCAbiValuePlan *plan = &signature->parameters[i];
        LLVMValueRef value = NULL;
        if (!plan->is_aggregate) {
            value = LLVMGetParam(state->function, plan->llvm_parameter_start);
        } else if (plan->kind == LLVM_C_ABI_INDIRECT) {
            LLVMValueRef ptr = LLVMGetParam(state->function, plan->llvm_parameter_start);
            value = LLVMBuildLoad2(backend->builder, plan->storage_type, ptr, "");
        } else {
            LLVMValueRef components[2] = {0};
            for (unsigned c = 0; c < plan->component_count; ++c)
                components[c] = LLVMGetParam(state->function, plan->llvm_parameter_start + c);
            value = join_aggregate_components(backend, plan, components);
        }
        if (!value)
            return 0;
        state->values[function->parameters[i]] = value;
    }
    return 1;
}

int llvm_lower_c_call(
    LlvmBackend *backend,
    const CogIrFunction *function,
    LlvmFunctionState *state,
    const CogIrInstruction *instruction,
    LLVMValueRef callee,
    const LlvmCAbiSignature *signature,
    LLVMValueRef *out_result
) {
    size_t fixed = signature->parameter_count;
    if (instruction->as.call.argument_count < fixed ||
        (!signature->is_variadic && instruction->as.call.argument_count != fixed)) {
        llvm_backend_error(backend, "C call argument count does not match lowered ABI signature");
        return 0;
    }

    size_t extra = instruction->as.call.argument_count - fixed;
    size_t capacity = signature->llvm_parameter_count + extra;
    LLVMValueRef *args = capacity ? calloc(capacity, sizeof(*args)) : NULL;
    if (capacity && !args) {
        llvm_backend_error(backend, "out of memory lowering C ABI call arguments");
        return 0;
    }
    size_t at = 0;
    LLVMValueRef return_slot = NULL;
    if (signature->result.is_sret) {
        return_slot = alloca_storage(backend, &signature->result);
        if (!return_slot) { free(args); return 0; }
        args[at++] = return_slot;
    }

    for (size_t i = 0; i < fixed; ++i) {
        CogIrValueId argument_id = instruction->as.call.arguments[i];
        LLVMValueRef value = state->values[argument_id];
        const LlvmCAbiValuePlan *plan = &signature->parameters[i];
        if (!value) { free(args); return 0; }
        if (!plan->is_aggregate &&
            abi_pointer_requires_distinct_object_storage(backend, plan->abi_type)) {
            const CogIrValue *argument = cog_ir_get_value(function, argument_id);
            if (!argument || argument->abi_type == COG_IR_ABI_TYPE_INVALID ||
                !abi_storage_spelling_compatible(backend, plan->abi_type, argument->abi_type)) {
                free(args);
                llvm_backend_error(backend,
                    "C pointer argument requires exact addressable C _Bool object storage");
                return 0;
            }
        }
        if (!plan->is_aggregate) {
            args[at++] = value;
        } else if (plan->kind == LLVM_C_ABI_INDIRECT) {
            LLVMValueRef slot = alloca_storage(backend, plan);
            if (!slot || !LLVMBuildStore(backend->builder, value, slot)) { free(args); return 0; }
            args[at++] = slot;
        } else {
            LLVMValueRef components[2] = {0};
            if (!split_aggregate_value(backend, plan, value, components)) { free(args); return 0; }
            for (unsigned c = 0; c < plan->component_count; ++c)
                args[at++] = components[c];
        }
    }
    for (size_t i = fixed; i < instruction->as.call.argument_count; ++i) {
        LLVMValueRef value = state->values[instruction->as.call.arguments[i]];
        if (!value) { free(args); return 0; }
        args[at++] = value;
    }

    LLVMValueRef call = LLVMBuildCall2(
        backend->builder, signature->function_type, callee,
        args, (unsigned)at, "");
    free(args);
    if (!call || !llvm_apply_c_call_abi(backend, call, signature))
        return 0;

    if (signature->result.is_sret) {
        *out_result = LLVMBuildLoad2(
            backend->builder, signature->result.storage_type, return_slot, "");
        return *out_result != NULL;
    }
    if (signature->result.is_aggregate) {
        LLVMValueRef components[2] = {0};
        if (signature->result.component_count == 1) {
            components[0] = call;
        } else {
            for (unsigned c = 0; c < signature->result.component_count; ++c)
                components[c] = LLVMBuildExtractValue(backend->builder, call, c, "");
        }
        *out_result = join_aggregate_components(backend, &signature->result, components);
        return *out_result != NULL;
    }
    *out_result = call;
    return 1;
}

int llvm_lower_c_return(
    LlvmBackend *backend,
    LlvmFunctionState *state,
    int has_value,
    LLVMValueRef value
) {
    const LlvmCAbiSignature *signature = state->c_abi;
    if (!signature) {
        llvm_backend_error(backend, "missing C ABI signature while lowering C function return");
        return 0;
    }
    if (!has_value) {
        LLVMBuildRetVoid(backend->builder);
        return 1;
    }
    if (!value) {
        llvm_backend_error(backend, "C function return references unavailable LLVM value");
        return 0;
    }
    if (signature->result.is_sret) {
        LLVMValueRef sret = LLVMGetParam(state->function, 0);
        if (!LLVMBuildStore(backend->builder, value, sret))
            return 0;
        LLVMBuildRetVoid(backend->builder);
        return 1;
    }
    if (signature->result.is_aggregate) {
        LLVMValueRef components[2] = {0};
        if (!split_aggregate_value(backend, &signature->result, value, components))
            return 0;
        if (signature->result.component_count == 1) {
            LLVMBuildRet(backend->builder, components[0]);
        } else {
            LLVMTypeRef return_type = LLVMGetReturnType(signature->function_type);
            LLVMValueRef result = LLVMGetUndef(return_type);
            for (unsigned c = 0; c < signature->result.component_count; ++c)
                result = LLVMBuildInsertValue(backend->builder, result, components[c], c, "");
            LLVMBuildRet(backend->builder, result);
        }
        return 1;
    }
    LLVMBuildRet(backend->builder, value);
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
