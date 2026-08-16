#include "backend_llvm_internal.h"

#include <stdio.h>
#include <stdlib.h>

static int runtime_is_repr_c_aggregate(const CogIrType *type)
{
    return type && (type->kind == COG_IR_TYPE_STRUCT || type->kind == COG_IR_TYPE_UNION) &&
           type->as.aggregate.is_repr_c;
}

static LLVMTypeRef c_bool_storage_type(LlvmBackend *backend)
{
    unsigned bits = backend->ir->target.c_bool_bits;
    if (!bits) {
        llvm_backend_error(backend, "target C _Bool storage width is unavailable");
        return NULL;
    }
    return LLVMIntTypeInContext(backend->context, bits);
}

LLVMTypeRef llvm_lower_c_object_type(LlvmBackend *backend, CogIrAbiTypeId id)
{
    const CogIrAbiType *abi = cog_ir_get_abi_type(backend->ir, id);
    if (!abi) {
        llvm_backend_error(backend, "invalid CogIR C object ABI type");
        return NULL;
    }
    const CogIrType *runtime = cog_ir_get_type(backend->ir, abi->runtime_type);
    if (!runtime) {
        llvm_backend_error(backend, "C object ABI type references an invalid runtime type");
        return NULL;
    }

    switch (abi->kind) {
        case COG_IR_ABI_TYPE_C_SCALAR:
            if (abi->c_scalar_kind == COG_IR_C_SCALAR_BOOL)
                return c_bool_storage_type(backend);
            return llvm_lower_type(backend, abi->runtime_type);

        case COG_IR_ABI_TYPE_SEMANTIC:
            if (runtime->kind == COG_IR_TYPE_BOOL)
                return c_bool_storage_type(backend);
            if (runtime->kind == COG_IR_TYPE_ENUM && runtime->as.enumeration.is_repr_c)
                return llvm_lower_c_object_type(backend, runtime->as.enumeration.backing_abi_type);
            if (runtime_is_repr_c_aggregate(runtime))
                return llvm_lower_type(backend, runtime->id);
            return llvm_lower_type(backend, abi->runtime_type);

        case COG_IR_ABI_TYPE_POINTER:
        case COG_IR_ABI_TYPE_OPAQUE_POINTER:
        case COG_IR_ABI_TYPE_FUNCTION:
            return LLVMPointerTypeInContext(backend->context, 0);

        case COG_IR_ABI_TYPE_ARRAY: {
            const CogIrType *array = runtime;
            if (array->kind != COG_IR_TYPE_ARRAY) {
                llvm_backend_error(backend, "C array ABI metadata references a non-array runtime type");
                return NULL;
            }
            LLVMTypeRef element = llvm_lower_c_object_type(backend, abi->element_type);
            return element ? LLVMArrayType2(element, (uint64_t)array->as.array.length) : NULL;
        }
    }

    llvm_backend_error(backend, "unsupported CogIR C object ABI type kind");
    return NULL;
}

static LLVMTypeRef alignment_marker_type(LlvmBackend *backend, unsigned alignment)
{
    if (alignment <= 1)
        return LLVMArrayType2(LLVMInt8TypeInContext(backend->context), 0);

    LLVMTypeRef vector = LLVMVectorType(
        LLVMInt8TypeInContext(backend->context), alignment);
    if (!vector || LLVMABIAlignmentOfType(backend->target_data, vector) != alignment) {
        llvm_backend_error(backend, "LLVM target cannot represent requested #repr(c) aggregate alignment");
        return NULL;
    }
    return LLVMArrayType2(vector, 0);
}

static LLVMTypeRef wrap_explicit_alignment(
    LlvmBackend *backend,
    const CogIrType *type,
    LLVMTypeRef inner
) {
    if (!type->as.aggregate.explicit_alignment)
        return inner;

    LLVMTypeRef marker = alignment_marker_type(
        backend, type->as.aggregate.explicit_alignment);
    if (!marker)
        return NULL;

    char name[80];
    snprintf(name, sizeof(name), "cog.c.aggregate.%u", type->id);
    LLVMTypeRef outer = LLVMStructCreateNamed(backend->context, name);
    if (!outer) {
        llvm_backend_error(backend, "could not create aligned #repr(c) aggregate wrapper");
        return NULL;
    }
    LLVMTypeRef fields[2] = { inner, marker };
    LLVMStructSetBody(outer, fields, 2, 0);
    backend->c_aggregate_inner_types[type->id] = inner;
    backend->c_aggregate_is_wrapped[type->id] = 1;
    return outer;
}

static LLVMTypeRef lower_repr_c_struct(LlvmBackend *backend, const CogIrType *type)
{
    char name[80];
    snprintf(
        name, sizeof(name),
        type->as.aggregate.explicit_alignment ? "cog.c.struct.inner.%u" : "cog.c.struct.%u",
        type->id);
    LLVMTypeRef inner = LLVMStructCreateNamed(backend->context, name);
    if (!inner) {
        llvm_backend_error(backend, "could not create LLVM #repr(c) struct type");
        return NULL;
    }

    LLVMTypeRef *fields = type->as.aggregate.field_count
        ? calloc(type->as.aggregate.field_count, sizeof(*fields)) : NULL;
    if (type->as.aggregate.field_count && !fields) {
        llvm_backend_error(backend, "out of memory lowering #repr(c) struct fields");
        return NULL;
    }
    for (size_t i = 0; i < type->as.aggregate.field_count; ++i) {
        CogIrAbiTypeId field_abi = type->as.aggregate.fields[i].abi_type;
        if (field_abi == COG_IR_ABI_TYPE_INVALID) {
            free(fields);
            llvm_backend_error(backend, "#repr(c) struct field is missing frozen C object ABI metadata");
            return NULL;
        }
        fields[i] = llvm_lower_c_object_type(backend, field_abi);
        if (!fields[i]) {
            free(fields);
            return NULL;
        }
    }
    LLVMStructSetBody(
        inner,
        fields,
        (unsigned)type->as.aggregate.field_count,
        type->as.aggregate.is_packed ? 1 : 0);
    free(fields);

    if (!type->as.aggregate.explicit_alignment) {
        backend->c_aggregate_inner_types[type->id] = inner;
        return inner;
    }
    return wrap_explicit_alignment(backend, type, inner);
}

static LLVMTypeRef lower_repr_c_union(LlvmBackend *backend, const CogIrType *type)
{
    uint64_t max_size = 0;
    unsigned max_align = 1;
    LLVMTypeRef carrier = NULL;

    for (size_t i = 0; i < type->as.aggregate.field_count; ++i) {
        CogIrAbiTypeId field_abi = type->as.aggregate.fields[i].abi_type;
        if (field_abi == COG_IR_ABI_TYPE_INVALID) {
            llvm_backend_error(backend, "#repr(c) union field is missing frozen C object ABI metadata");
            return NULL;
        }
        LLVMTypeRef field = llvm_lower_c_object_type(backend, field_abi);
        if (!field)
            return NULL;
        uint64_t size = LLVMABISizeOfType(backend->target_data, field);
        unsigned align = LLVMABIAlignmentOfType(backend->target_data, field);
        if (size > max_size)
            max_size = size;
        if (align > max_align) {
            max_align = align;
            carrier = field;
        } else if (!carrier) {
            carrier = field;
        }
    }

    char name[80];
    snprintf(
        name, sizeof(name),
        type->as.aggregate.explicit_alignment ? "cog.c.union.inner.%u" : "cog.c.union.%u",
        type->id);
    LLVMTypeRef inner = LLVMStructCreateNamed(backend->context, name);
    if (!inner) {
        llvm_backend_error(backend, "could not create LLVM #repr(c) union type");
        return NULL;
    }

    if (type->as.aggregate.is_packed) {
        LLVMTypeRef bytes = LLVMArrayType2(
            LLVMInt8TypeInContext(backend->context), max_size);
        LLVMStructSetBody(inner, &bytes, 1, 1);
    } else {
        uint64_t carrier_size = LLVMABISizeOfType(backend->target_data, carrier);
        LLVMTypeRef fields[2];
        unsigned count = 1;
        fields[0] = carrier;
        if (carrier_size < max_size) {
            fields[1] = LLVMArrayType2(
                LLVMInt8TypeInContext(backend->context), max_size - carrier_size);
            count = 2;
        }
        LLVMStructSetBody(inner, fields, count, 0);
    }

    if (!type->as.aggregate.explicit_alignment) {
        backend->c_aggregate_inner_types[type->id] = inner;
        return inner;
    }
    return wrap_explicit_alignment(backend, type, inner);
}

LLVMTypeRef llvm_lower_repr_c_aggregate_type(LlvmBackend *backend, const CogIrType *type)
{
    if (!runtime_is_repr_c_aggregate(type) || !type->as.aggregate.is_complete ||
        type->as.aggregate.is_incomplete) {
        llvm_backend_error(backend, "invalid complete #repr(c) aggregate type");
        return NULL;
    }

    LLVMTypeRef result = type->kind == COG_IR_TYPE_STRUCT
        ? lower_repr_c_struct(backend, type)
        : lower_repr_c_union(backend, type);
    if (!result)
        return NULL;
    backend->types[type->id] = result;
    if (!backend->c_aggregate_inner_types[type->id])
        backend->c_aggregate_inner_types[type->id] = result;
    return result;
}

LLVMTypeRef llvm_repr_c_inner_type(LlvmBackend *backend, CogIrTypeId id)
{
    LLVMTypeRef type = llvm_lower_type(backend, id);
    if (!type)
        return NULL;
    return backend->c_aggregate_inner_types[id]
        ? backend->c_aggregate_inner_types[id] : type;
}

int llvm_repr_c_is_wrapped(LlvmBackend *backend, CogIrTypeId id)
{
    if (!llvm_lower_type(backend, id))
        return 0;
    return backend->c_aggregate_is_wrapped[id] != 0;
}

uint64_t llvm_repr_c_field_offset(
    LlvmBackend *backend,
    const CogIrType *type,
    size_t field_index
) {
    if (!type || type->kind != COG_IR_TYPE_STRUCT || !type->as.aggregate.is_repr_c ||
        field_index >= type->as.aggregate.field_count)
        return UINT64_MAX;
    LLVMTypeRef inner = llvm_repr_c_inner_type(backend, type->id);
    if (!inner)
        return UINT64_MAX;
    return LLVMOffsetOfElement(backend->target_data, inner, (unsigned)field_index);
}

LLVMValueRef llvm_build_repr_c_field_gep(
    LlvmBackend *backend,
    const CogIrType *type,
    LLVMValueRef base,
    size_t field_index
) {
    if (!type || type->kind != COG_IR_TYPE_STRUCT || !type->as.aggregate.is_repr_c ||
        field_index >= type->as.aggregate.field_count) {
        llvm_backend_error(backend, "invalid #repr(c) struct field address");
        return NULL;
    }
    LLVMValueRef inner_ptr = base;
    if (llvm_repr_c_is_wrapped(backend, type->id)) {
        LLVMTypeRef outer = llvm_lower_type(backend, type->id);
        inner_ptr = LLVMBuildStructGEP2(backend->builder, outer, base, 0, "");
        if (!inner_ptr)
            return NULL;
    }
    LLVMTypeRef inner = llvm_repr_c_inner_type(backend, type->id);
    return inner ? LLVMBuildStructGEP2(
        backend->builder, inner, inner_ptr, (unsigned)field_index, "") : NULL;
}

static LLVMValueRef zext_or_trunc_bool_storage(
    LlvmBackend *backend,
    LLVMValueRef value,
    LLVMTypeRef target,
    int to_storage
) {
    if (to_storage)
        return LLVMBuildZExt(backend->builder, value, target, "");
    return LLVMBuildTrunc(backend->builder, value, target, "");
}

LLVMValueRef llvm_c_runtime_to_object(
    LlvmBackend *backend,
    CogIrAbiTypeId abi_id,
    LLVMValueRef runtime_value
) {
    const CogIrAbiType *abi = cog_ir_get_abi_type(backend->ir, abi_id);
    if (!abi || !runtime_value)
        return NULL;
    const CogIrType *runtime = cog_ir_get_type(backend->ir, abi->runtime_type);
    if (!runtime)
        return NULL;

    if ((abi->kind == COG_IR_ABI_TYPE_C_SCALAR && abi->c_scalar_kind == COG_IR_C_SCALAR_BOOL) ||
        (abi->kind == COG_IR_ABI_TYPE_SEMANTIC && runtime->kind == COG_IR_TYPE_BOOL)) {
        LLVMTypeRef storage = llvm_lower_c_object_type(backend, abi_id);
        return storage ? zext_or_trunc_bool_storage(backend, runtime_value, storage, 1) : NULL;
    }

    if (abi->kind == COG_IR_ABI_TYPE_ARRAY) {
        const CogIrType *array = runtime;
        LLVMTypeRef storage_type = llvm_lower_c_object_type(backend, abi_id);
        if (!storage_type || array->kind != COG_IR_TYPE_ARRAY)
            return NULL;
        LLVMValueRef result = LLVMGetUndef(storage_type);
        for (size_t i = 0; i < array->as.array.length; ++i) {
            LLVMValueRef element = LLVMBuildExtractValue(
                backend->builder, runtime_value, (unsigned)i, "");
            LLVMValueRef stored = llvm_c_runtime_to_object(
                backend, abi->element_type, element);
            if (!stored)
                return NULL;
            result = LLVMBuildInsertValue(
                backend->builder, result, stored, (unsigned)i, "");
        }
        return result;
    }

    return runtime_value;
}

LLVMValueRef llvm_c_object_to_runtime(
    LlvmBackend *backend,
    CogIrAbiTypeId abi_id,
    LLVMValueRef storage_value
) {
    const CogIrAbiType *abi = cog_ir_get_abi_type(backend->ir, abi_id);
    if (!abi || !storage_value)
        return NULL;
    const CogIrType *runtime = cog_ir_get_type(backend->ir, abi->runtime_type);
    if (!runtime)
        return NULL;

    if ((abi->kind == COG_IR_ABI_TYPE_C_SCALAR && abi->c_scalar_kind == COG_IR_C_SCALAR_BOOL) ||
        (abi->kind == COG_IR_ABI_TYPE_SEMANTIC && runtime->kind == COG_IR_TYPE_BOOL)) {
        LLVMTypeRef logical = llvm_lower_type(backend, abi->runtime_type);
        return logical ? zext_or_trunc_bool_storage(backend, storage_value, logical, 0) : NULL;
    }

    if (abi->kind == COG_IR_ABI_TYPE_ARRAY) {
        const CogIrType *array = runtime;
        LLVMTypeRef logical_type = llvm_lower_type(backend, abi->runtime_type);
        if (!logical_type || array->kind != COG_IR_TYPE_ARRAY)
            return NULL;
        LLVMValueRef result = LLVMGetUndef(logical_type);
        for (size_t i = 0; i < array->as.array.length; ++i) {
            LLVMValueRef element = LLVMBuildExtractValue(
                backend->builder, storage_value, (unsigned)i, "");
            LLVMValueRef logical = llvm_c_object_to_runtime(
                backend, abi->element_type, element);
            if (!logical)
                return NULL;
            result = LLVMBuildInsertValue(
                backend->builder, result, logical, (unsigned)i, "");
        }
        return result;
    }

    return storage_value;
}
