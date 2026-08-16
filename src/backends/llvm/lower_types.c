#include "backend_llvm_internal.h"

#include <stdio.h>
#include <stdlib.h>

LLVMTypeRef llvm_lower_function_signature(LlvmBackend *backend, CogIrTypeId id)
{
    const CogIrType *type = cog_ir_get_type(backend->ir, id);
    if (!type || type->kind != COG_IR_TYPE_FUNCTION) {
        llvm_backend_error(backend, "function signature references a non-function CogIR type");
        return NULL;
    }
    if (type->as.function.abi != COG_IR_ABI_COGLET || type->as.function.is_variadic) {
        llvm_backend_error(backend, "Stage 4 supports callable signatures only for non-variadic Coglet ABI functions");
        return NULL;
    }

    LLVMTypeRef result = llvm_lower_type(backend, type->as.function.result_type);
    if (!result)
        return NULL;

    LLVMTypeRef *params = NULL;
    if (type->as.function.parameter_count) {
        params = calloc(type->as.function.parameter_count, sizeof(*params));
        if (!params) {
            llvm_backend_error(backend, "out of memory lowering function signature");
            return NULL;
        }
        for (size_t i = 0; i < type->as.function.parameter_count; ++i) {
            params[i] = llvm_lower_type(backend, type->as.function.parameter_types[i]);
            if (!params[i]) {
                free(params);
                return NULL;
            }
        }
    }

    LLVMTypeRef fn = LLVMFunctionType(result, params, (unsigned)type->as.function.parameter_count, 0);
    free(params);
    return fn;
}

static LLVMTypeRef lower_struct_type(LlvmBackend *backend, const CogIrType *type)
{
    if (type->as.aggregate.is_repr_c) {
        llvm_backend_error(backend, "#repr(c) aggregate layout is outside the LLVM Stage 3 subset");
        return NULL;
    }
    if (!type->as.aggregate.is_complete || type->as.aggregate.is_incomplete) {
        llvm_backend_error(backend, "incomplete aggregate has no LLVM value layout");
        return NULL;
    }

    char name[64];
    snprintf(name, sizeof(name), "cog.struct.%u", type->id);
    LLVMTypeRef result = LLVMStructCreateNamed(backend->context, name);
    if (!result) {
        llvm_backend_error(backend, "could not create LLVM struct type");
        return NULL;
    }

    /* Cache nominal identity before lowering fields so pointer-recursive graphs
     * never attempt to create a second LLVM struct for the same CogIR type. */
    backend->types[type->id] = result;

    LLVMTypeRef *fields = NULL;
    if (type->as.aggregate.field_count) {
        fields = calloc(type->as.aggregate.field_count, sizeof(*fields));
        if (!fields) {
            llvm_backend_error(backend, "out of memory lowering struct fields");
            backend->types[type->id] = NULL;
            return NULL;
        }
        for (size_t i = 0; i < type->as.aggregate.field_count; ++i) {
            fields[i] = llvm_lower_type(backend, type->as.aggregate.fields[i].type);
            if (!fields[i]) {
                free(fields);
                backend->types[type->id] = NULL;
                return NULL;
            }
        }
    }

    LLVMStructSetBody(result, fields, (unsigned)type->as.aggregate.field_count, 0);
    free(fields);
    return result;
}

LLVMTypeRef llvm_lower_type(LlvmBackend *backend, CogIrTypeId id)
{
    if (id == COG_IR_TYPE_INVALID || (size_t)id >= backend->ir->type_count) {
        llvm_backend_error(backend, "invalid CogIR type id");
        return NULL;
    }
    if (backend->types[id])
        return backend->types[id];

    const CogIrType *type = cog_ir_get_type(backend->ir, id);
    LLVMTypeRef result = NULL;
    switch (type->kind) {
        case COG_IR_TYPE_VOID:
            result = LLVMVoidTypeInContext(backend->context);
            break;
        case COG_IR_TYPE_BOOL:
            result = LLVMIntTypeInContext(backend->context, 1);
            break;
        case COG_IR_TYPE_INTEGER:
            result = LLVMIntTypeInContext(backend->context, type->as.integer.bits);
            break;
        case COG_IR_TYPE_POINTER:
        case COG_IR_TYPE_OPAQUE_POINTER:
            result = LLVMPointerTypeInContext(backend->context, 0);
            break;
        case COG_IR_TYPE_ARRAY: {
            LLVMTypeRef element = llvm_lower_type(backend, type->as.array.element_type);
            if (!element)
                return NULL;
            result = LLVMArrayType2(element, (uint64_t)type->as.array.length);
            break;
        }
        case COG_IR_TYPE_STRUCT:
            return lower_struct_type(backend, type);
        case COG_IR_TYPE_FUNCTION:
            /* Function values are first-class opaque pointers in LLVM. Their
             * callable signature is lowered separately for declarations/calls. */
            result = LLVMPointerTypeInContext(backend->context, 0);
            break;
        case COG_IR_TYPE_ENUM:
            result = llvm_lower_type(backend, type->as.enumeration.backing_type);
            if (!result)
                return NULL;
            break;
        case COG_IR_TYPE_FLOAT:
            if (type->as.floating.bits == 32)
                result = LLVMFloatTypeInContext(backend->context);
            else if (type->as.floating.bits == 64)
                result = LLVMDoubleTypeInContext(backend->context);
            else {
                llvm_backend_error(backend, "unsupported CogIR floating-point width");
                return NULL;
            }
            break;
        case COG_IR_TYPE_UNION:
            llvm_backend_error(backend, "union layout is outside the LLVM Stage 3 subset");
            return NULL;
    }
    backend->types[id] = result;
    return result;
}

static LLVMValueRef *lower_constant_elements(
    LlvmBackend *backend,
    const CogIrConstant *constant
) {
    if (!constant->as.aggregate.element_count)
        return NULL;

    LLVMValueRef *values = calloc(constant->as.aggregate.element_count, sizeof(*values));
    if (!values) {
        llvm_backend_error(backend, "out of memory lowering aggregate constant");
        return NULL;
    }
    for (size_t i = 0; i < constant->as.aggregate.element_count; ++i) {
        values[i] = llvm_lower_constant(backend, constant->as.aggregate.elements[i]);
        if (!values[i]) {
            free(values);
            return NULL;
        }
    }
    return values;
}

LLVMValueRef llvm_lower_constant(LlvmBackend *backend, CogIrConstId id)
{
    const CogIrConstant *constant = cog_ir_get_constant(backend->ir, id);
    if (!constant) {
        llvm_backend_error(backend, "invalid CogIR constant id");
        return NULL;
    }
    LLVMTypeRef type = llvm_lower_type(backend, constant->type);
    if (!type)
        return NULL;

    switch (constant->kind) {
        case COG_IR_CONST_ZERO:
            return LLVMConstNull(type);
        case COG_IR_CONST_BOOL:
            return LLVMConstInt(type, constant->as.boolean ? 1 : 0, 0);
        case COG_IR_CONST_INTEGER:
            return LLVMConstInt(type, constant->as.integer_bits, 0);
        case COG_IR_CONST_NULL:
            return LLVMConstNull(type);
        case COG_IR_CONST_ARRAY: {
            const CogIrType *array = cog_ir_get_type(backend->ir, constant->type);
            if (!array || array->kind != COG_IR_TYPE_ARRAY) {
                llvm_backend_error(backend, "array constant has non-array CogIR type");
                return NULL;
            }
            LLVMValueRef *values = lower_constant_elements(backend, constant);
            if (constant->as.aggregate.element_count && !values)
                return NULL;
            LLVMTypeRef element = llvm_lower_type(backend, array->as.array.element_type);
            LLVMValueRef result = element
                ? LLVMConstArray2(element, values, (uint64_t)constant->as.aggregate.element_count)
                : NULL;
            free(values);
            return result;
        }
        case COG_IR_CONST_STRUCT: {
            const CogIrType *structure = cog_ir_get_type(backend->ir, constant->type);
            if (!structure || structure->kind != COG_IR_TYPE_STRUCT || structure->as.aggregate.is_repr_c) {
                llvm_backend_error(backend, "struct constant is outside the LLVM Stage 3 subset");
                return NULL;
            }
            LLVMValueRef *values = lower_constant_elements(backend, constant);
            if (constant->as.aggregate.element_count && !values)
                return NULL;
            LLVMValueRef result = LLVMConstNamedStruct(
                type,
                values,
                (unsigned)constant->as.aggregate.element_count
            );
            free(values);
            return result;
        }
        case COG_IR_CONST_FLOAT32: {
            LLVMTypeRef bits_type = LLVMIntTypeInContext(backend->context, 32);
            LLVMValueRef bits = LLVMConstInt(bits_type, constant->as.float32_bits, 0);
            return LLVMConstBitCast(bits, type);
        }
        case COG_IR_CONST_FLOAT64: {
            LLVMTypeRef bits_type = LLVMIntTypeInContext(backend->context, 64);
            LLVMValueRef bits = LLVMConstInt(bits_type, constant->as.float64_bits, 0);
            return LLVMConstBitCast(bits, type);
        }
    }
    return NULL;
}
