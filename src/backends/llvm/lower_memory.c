#include "backend_llvm_internal.h"

#include <stdlib.h>

static const CogIrType *value_type(
    LlvmBackend *backend,
    const CogIrFunction *function,
    CogIrValueId value
) {
    const CogIrValue *ir_value = cog_ir_get_value(function, value);
    return ir_value ? cog_ir_get_type(backend->ir, ir_value->type) : NULL;
}

static const CogIrType *typed_pointer_pointee(
    LlvmBackend *backend,
    const CogIrFunction *function,
    CogIrValueId value
) {
    const CogIrType *pointer = value_type(backend, function, value);
    if (!pointer || pointer->kind != COG_IR_TYPE_POINTER)
        return NULL;
    return cog_ir_get_type(backend->ir, pointer->as.pointer.pointee);
}

static const CogIrAbiType *address_element_abi(
    LlvmBackend *backend,
    const CogIrFunction *function,
    CogIrValueId address
) {
    const CogIrValue *value = cog_ir_get_value(function, address);
    if (!value || value->abi_type == COG_IR_ABI_TYPE_INVALID)
        return NULL;
    const CogIrAbiType *pointer = cog_ir_get_abi_type(backend->ir, value->abi_type);
    if (!pointer || pointer->kind != COG_IR_ABI_TYPE_POINTER)
        return NULL;
    return cog_ir_get_abi_type(backend->ir, pointer->element_type);
}

static int lower_field_addr(
    LlvmBackend *backend,
    const CogIrFunction *function,
    LlvmFunctionState *state,
    const CogIrInstruction *instruction,
    LLVMValueRef *out_result
) {
    LLVMValueRef base = state->values[instruction->as.field_addr.base];
    const CogIrType *aggregate = typed_pointer_pointee(
        backend,
        function,
        instruction->as.field_addr.base
    );
    if (!base || !aggregate || aggregate->kind != COG_IR_TYPE_STRUCT ||
        instruction->as.field_addr.field_index >= aggregate->as.aggregate.field_count) {
        llvm_backend_error(backend, "field_addr requires a supported Coglet struct base");
        return 0;
    }

    if (aggregate->as.aggregate.is_repr_c) {
        *out_result = llvm_build_repr_c_field_gep(
            backend, aggregate, base, instruction->as.field_addr.field_index);
    } else {
        LLVMTypeRef llvm_aggregate = llvm_lower_type(backend, aggregate->id);
        if (!llvm_aggregate)
            return 0;
        *out_result = LLVMBuildStructGEP2(
            backend->builder,
            llvm_aggregate,
            base,
            instruction->as.field_addr.field_index,
            ""
        );
    }
    return *out_result != NULL;
}

static int lower_array_elem_addr(
    LlvmBackend *backend,
    const CogIrFunction *function,
    LlvmFunctionState *state,
    const CogIrInstruction *instruction,
    LLVMValueRef *out_result
) {
    LLVMValueRef base = state->values[instruction->as.index_addr.base];
    LLVMValueRef index = state->values[instruction->as.index_addr.index];
    const CogIrType *array = typed_pointer_pointee(
        backend,
        function,
        instruction->as.index_addr.base
    );
    if (!base || !index || !array || array->kind != COG_IR_TYPE_ARRAY) {
        llvm_backend_error(backend, "array_elem_addr requires a typed pointer to a Coglet array");
        return 0;
    }

    LLVMTypeRef llvm_array = NULL;
    const CogIrAbiType *element_abi = address_element_abi(
        backend, function, instruction->as.index_addr.base);
    if (element_abi && element_abi->kind == COG_IR_ABI_TYPE_ARRAY)
        llvm_array = llvm_lower_c_object_type(backend, element_abi->id);
    else
        llvm_array = llvm_lower_type(backend, array->id);
    if (!llvm_array)
        return 0;
    LLVMValueRef indices[2] = {
        LLVMConstInt(LLVMIntTypeInContext(backend->context, 32), 0, 0),
        index,
    };
    *out_result = LLVMBuildGEP2(backend->builder, llvm_array, base, indices, 2, "");
    return *out_result != NULL;
}

static int lower_ptr_index_addr(
    LlvmBackend *backend,
    const CogIrFunction *function,
    LlvmFunctionState *state,
    const CogIrInstruction *instruction,
    LLVMValueRef *out_result
) {
    LLVMValueRef base = state->values[instruction->as.index_addr.base];
    LLVMValueRef index = state->values[instruction->as.index_addr.index];
    const CogIrType *pointee = typed_pointer_pointee(
        backend,
        function,
        instruction->as.index_addr.base
    );
    if (!base || !index || !pointee) {
        llvm_backend_error(backend, "ptr_index_addr requires a typed pointer and integer index");
        return 0;
    }

    LLVMTypeRef llvm_pointee = NULL;
    const CogIrAbiType *element_abi = address_element_abi(
        backend, function, instruction->as.index_addr.base);
    if (element_abi)
        llvm_pointee = llvm_lower_c_object_type(backend, element_abi->id);
    else
        llvm_pointee = llvm_lower_type(backend, pointee->id);
    if (!llvm_pointee)
        return 0;
    *out_result = LLVMBuildGEP2(backend->builder, llvm_pointee, base, &index, 1, "");
    return *out_result != NULL;
}

static int lower_load(
    LlvmBackend *backend,
    const CogIrFunction *function,
    const CogIrInstruction *instruction,
    LlvmFunctionState *state,
    LLVMValueRef *out_result
) {
    const CogIrType *ir_type = cog_ir_get_type(backend->ir, instruction->result_type);
    if (instruction->as.load.is_volatile && ir_type &&
        (ir_type->kind == COG_IR_TYPE_ARRAY || ir_type->kind == COG_IR_TYPE_STRUCT ||
         ir_type->kind == COG_IR_TYPE_UNION)) {
        llvm_backend_error(backend, "volatile aggregate loads remain unsupported in the LLVM backend");
        return 0;
    }
    LLVMValueRef address = state->values[instruction->as.load.address];
    const CogIrAbiType *object_abi = address_element_abi(
        backend, function, instruction->as.load.address);
    LLVMTypeRef type = object_abi
        ? llvm_lower_c_object_type(backend, object_abi->id)
        : llvm_lower_type(backend, instruction->result_type);
    if (!address || !type) {
        llvm_backend_error(backend, "load references unavailable LLVM address or type");
        return 0;
    }
    LLVMValueRef loaded = LLVMBuildLoad2(backend->builder, type, address, "");
    if (!loaded)
        return 0;
    if (instruction->as.load.is_volatile)
        LLVMSetVolatile(loaded, 1);
    *out_result = object_abi
        ? llvm_c_object_to_runtime(backend, object_abi->id, loaded)
        : loaded;
    return *out_result != NULL;
}

static int lower_store(
    LlvmBackend *backend,
    const CogIrFunction *function,
    const CogIrInstruction *instruction,
    LlvmFunctionState *state,
    LLVMValueRef *out_result
) {
    const CogIrType *ir_type = value_type(backend, function, instruction->as.store.value);
    if (instruction->as.store.is_volatile && ir_type &&
        (ir_type->kind == COG_IR_TYPE_ARRAY || ir_type->kind == COG_IR_TYPE_STRUCT ||
         ir_type->kind == COG_IR_TYPE_UNION)) {
        llvm_backend_error(backend, "volatile aggregate stores remain unsupported in the LLVM backend");
        return 0;
    }
    LLVMValueRef address = state->values[instruction->as.store.address];
    LLVMValueRef value = state->values[instruction->as.store.value];
    const CogIrAbiType *object_abi = address_element_abi(
        backend, function, instruction->as.store.address);
    if (!address || !value) {
        llvm_backend_error(backend, "store references unavailable LLVM value");
        return 0;
    }
    if (object_abi) {
        value = llvm_c_runtime_to_object(backend, object_abi->id, value);
        if (!value)
            return 0;
    }
    *out_result = LLVMBuildStore(backend->builder, value, address);
    if (!*out_result)
        return 0;
    if (instruction->as.store.is_volatile)
        LLVMSetVolatile(*out_result, 1);
    return 1;
}

static int lower_make_aggregate(
    LlvmBackend *backend,
    LlvmFunctionState *state,
    const CogIrInstruction *instruction,
    LLVMValueRef *out_result
) {
    LLVMTypeRef type = llvm_lower_type(backend, instruction->result_type);
    if (!type)
        return 0;

    LLVMValueRef result = LLVMGetUndef(type);
    if (!result) {
        llvm_backend_error(backend, "could not create LLVM aggregate value");
        return 0;
    }
    for (size_t i = 0; i < instruction->as.aggregate.value_count; ++i) {
        LLVMValueRef element = state->values[instruction->as.aggregate.values[i]];
        if (!element) {
            llvm_backend_error(backend, "aggregate construction references unavailable LLVM value");
            return 0;
        }
        result = LLVMBuildInsertValue(backend->builder, result, element, (unsigned)i, "");
        if (!result)
            return 0;
    }
    *out_result = result;
    return 1;
}

static int lower_extract_aggregate(
    LlvmBackend *backend,
    LlvmFunctionState *state,
    const CogIrInstruction *instruction,
    LLVMValueRef *out_result
) {
    LLVMValueRef aggregate = state->values[instruction->as.extract.aggregate];
    if (!aggregate) {
        llvm_backend_error(backend, "aggregate extraction references unavailable LLVM value");
        return 0;
    }
    *out_result = LLVMBuildExtractValue(
        backend->builder,
        aggregate,
        instruction->as.extract.index,
        ""
    );
    return *out_result != NULL;
}

static int lower_make_repr_c_struct(
    LlvmBackend *backend,
    LlvmFunctionState *state,
    const CogIrInstruction *instruction,
    const CogIrType *type,
    LLVMValueRef *out_result
) {
    LLVMTypeRef inner_type = llvm_repr_c_inner_type(backend, type->id);
    LLVMTypeRef outer_type = llvm_lower_type(backend, type->id);
    if (!inner_type || !outer_type)
        return 0;
    LLVMValueRef inner = LLVMGetUndef(inner_type);
    for (size_t i = 0; i < instruction->as.aggregate.value_count; ++i) {
        LLVMValueRef value = state->values[instruction->as.aggregate.values[i]];
        CogIrAbiTypeId field_abi = type->as.aggregate.fields[i].abi_type;
        if (!value || field_abi == COG_IR_ABI_TYPE_INVALID) {
            llvm_backend_error(backend, "#repr(c) struct construction is missing field ABI metadata");
            return 0;
        }
        value = llvm_c_runtime_to_object(backend, field_abi, value);
        if (!value)
            return 0;
        inner = LLVMBuildInsertValue(backend->builder, inner, value, (unsigned)i, "");
        if (!inner)
            return 0;
    }
    if (!llvm_repr_c_is_wrapped(backend, type->id)) {
        *out_result = inner;
        return 1;
    }
    LLVMValueRef outer = LLVMGetUndef(outer_type);
    outer = LLVMBuildInsertValue(backend->builder, outer, inner, 0, "");
    *out_result = outer;
    return outer != NULL;
}

static int lower_extract_repr_c_field(
    LlvmBackend *backend,
    LlvmFunctionState *state,
    const CogIrInstruction *instruction,
    const CogIrType *type,
    LLVMValueRef *out_result
) {
    LLVMValueRef aggregate = state->values[instruction->as.extract.aggregate];
    if (!aggregate || instruction->as.extract.index >= type->as.aggregate.field_count)
        return 0;
    LLVMValueRef inner = aggregate;
    if (llvm_repr_c_is_wrapped(backend, type->id))
        inner = LLVMBuildExtractValue(backend->builder, aggregate, 0, "");
    LLVMValueRef storage = LLVMBuildExtractValue(
        backend->builder, inner, instruction->as.extract.index, "");
    CogIrAbiTypeId field_abi = type->as.aggregate.fields[instruction->as.extract.index].abi_type;
    if (!storage || field_abi == COG_IR_ABI_TYPE_INVALID) {
        llvm_backend_error(backend, "#repr(c) field extraction is missing field ABI metadata");
        return 0;
    }
    *out_result = llvm_c_object_to_runtime(backend, field_abi, storage);
    return *out_result != NULL;
}

int llvm_lower_memory_instruction(
    LlvmBackend *backend,
    const CogIrFunction *function,
    LlvmFunctionState *state,
    const CogIrInstruction *instruction,
    LLVMValueRef *out_result
) {
    switch (instruction->op) {
        case COG_IR_OP_LOCAL_ADDR:
            if ((size_t)instruction->as.local_addr.slot >= function->slot_count ||
                !state->slots[instruction->as.local_addr.slot]) {
                llvm_backend_error(backend, "local_addr references invalid LLVM slot");
                return 0;
            }
            *out_result = state->slots[instruction->as.local_addr.slot];
            return 1;

        case COG_IR_OP_GLOBAL_ADDR:
            if ((size_t)instruction->as.global_addr.global >= backend->ir->global_count ||
                !backend->globals[instruction->as.global_addr.global]) {
                llvm_backend_error(backend, "global_addr references invalid LLVM global");
                return 0;
            }
            *out_result = backend->globals[instruction->as.global_addr.global];
            return 1;

        case COG_IR_OP_FIELD_ADDR:
            return lower_field_addr(backend, function, state, instruction, out_result);
        case COG_IR_OP_ARRAY_ELEM_ADDR:
            return lower_array_elem_addr(backend, function, state, instruction, out_result);
        case COG_IR_OP_PTR_INDEX_ADDR:
            return lower_ptr_index_addr(backend, function, state, instruction, out_result);
        case COG_IR_OP_LOAD:
            return lower_load(backend, function, instruction, state, out_result);
        case COG_IR_OP_STORE:
            return lower_store(backend, function, instruction, state, out_result);

        case COG_IR_OP_MAKE_STRUCT: {
            const CogIrType *type = cog_ir_get_type(backend->ir, instruction->result_type);
            if (!type || type->kind != COG_IR_TYPE_STRUCT) {
                llvm_backend_error(backend, "make_struct requires a Coglet struct");
                return 0;
            }
            if (type->as.aggregate.is_repr_c)
                return lower_make_repr_c_struct(backend, state, instruction, type, out_result);
            return lower_make_aggregate(backend, state, instruction, out_result);
        }
        case COG_IR_OP_MAKE_ARRAY:
            return lower_make_aggregate(backend, state, instruction, out_result);
        case COG_IR_OP_EXTRACT_FIELD: {
            const CogIrValue *source = cog_ir_get_value(function, instruction->as.extract.aggregate);
            const CogIrType *type = source ? cog_ir_get_type(backend->ir, source->type) : NULL;
            if (!type || type->kind != COG_IR_TYPE_STRUCT) {
                llvm_backend_error(backend, "extract_field requires a Coglet struct");
                return 0;
            }
            if (type->as.aggregate.is_repr_c)
                return lower_extract_repr_c_field(backend, state, instruction, type, out_result);
            return lower_extract_aggregate(backend, state, instruction, out_result);
        }
        case COG_IR_OP_EXTRACT_ELEMENT:
            return lower_extract_aggregate(backend, state, instruction, out_result);

        case COG_IR_OP_PTR_EQ:
        case COG_IR_OP_PTR_NE: {
            LLVMValueRef lhs = state->values[instruction->as.binary.lhs];
            LLVMValueRef rhs = state->values[instruction->as.binary.rhs];
            if (!lhs || !rhs) {
                llvm_backend_error(backend, "pointer comparison references unavailable LLVM value");
                return 0;
            }
            *out_result = LLVMBuildICmp(
                backend->builder,
                instruction->op == COG_IR_OP_PTR_EQ ? LLVMIntEQ : LLVMIntNE,
                lhs,
                rhs,
                ""
            );
            return *out_result != NULL;
        }

        case COG_IR_OP_PTR_REINTERPRET:
        case COG_IR_OP_PTR_QUALIFY: {
            LLVMValueRef operand = state->values[instruction->as.conversion.operand];
            if (!operand) {
                llvm_backend_error(backend, "pointer conversion references unavailable LLVM value");
                return 0;
            }
            /* Coglet raw pointers currently lower into LLVM address-space-zero
             * opaque pointers. Qualifier changes and typed/opaque reinterpretation
             * therefore change CogIR type identity without requiring an LLVM op. */
            *out_result = operand;
            return 1;
        }

        default:
            llvm_backend_error(backend, "invalid memory operation passed to LLVM Stage 3 memory lowering");
            return 0;
    }
}
