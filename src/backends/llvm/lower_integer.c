#include "backend_llvm_internal.h"

#include <stdint.h>

static const CogIrType *integer_runtime_type_for_value(
    LlvmBackend *backend,
    const CogIrFunction *function,
    CogIrValueId value_id
) {
    const CogIrValue *value = cog_ir_get_value(function, value_id);
    const CogIrType *type = value ? cog_ir_get_type(backend->ir, value->type) : NULL;
    if (!type)
        return NULL;
    if (type->kind == COG_IR_TYPE_INTEGER)
        return type;
    if (type->kind == COG_IR_TYPE_ENUM) {
        const CogIrType *backing = cog_ir_get_type(backend->ir, type->as.enumeration.backing_type);
        return backing && backing->kind == COG_IR_TYPE_INTEGER ? backing : NULL;
    }
    return NULL;
}

static LLVMValueRef build_or_condition(LlvmBackend *backend, LLVMValueRef left, LLVMValueRef right)
{
    if (!left) return right;
    if (!right) return left;
    return LLVMBuildOr(backend->builder, left, right, "");
}

static int lower_overflow_checked_binary(
    LlvmBackend *backend,
    LlvmFunctionState *state,
    const CogIrType *type,
    CogIrOp op,
    LLVMValueRef lhs,
    LLVMValueRef rhs,
    LLVMValueRef *out_result
) {
    const char *name = NULL;
    if (type->as.integer.is_signed) {
        switch (op) {
            case COG_IR_OP_IADD_CHECKED: name = "llvm.sadd.with.overflow"; break;
            case COG_IR_OP_ISUB_CHECKED: name = "llvm.ssub.with.overflow"; break;
            case COG_IR_OP_IMUL_CHECKED: name = "llvm.smul.with.overflow"; break;
            default: break;
        }
    } else {
        switch (op) {
            case COG_IR_OP_IADD_CHECKED: name = "llvm.uadd.with.overflow"; break;
            case COG_IR_OP_ISUB_CHECKED: name = "llvm.usub.with.overflow"; break;
            case COG_IR_OP_IMUL_CHECKED: name = "llvm.umul.with.overflow"; break;
            default: break;
        }
    }
    if (!name) {
        llvm_backend_error(backend, "invalid checked integer overflow operation");
        return 0;
    }

    LLVMTypeRef integer_type = LLVMTypeOf(lhs);
    LLVMValueRef overflow_function = NULL;
    LLVMTypeRef overflow_function_type = NULL;
    if (!llvm_get_intrinsic(
            backend,
            name,
            &integer_type,
            1,
            &overflow_function,
            &overflow_function_type
        )) {
        return 0;
    }

    LLVMValueRef args[2] = { lhs, rhs };
    LLVMValueRef pair = LLVMBuildCall2(
        backend->builder,
        overflow_function_type,
        overflow_function,
        args,
        2,
        ""
    );
    LLVMValueRef result = LLVMBuildExtractValue(backend->builder, pair, 0, "");
    LLVMValueRef overflow = LLVMBuildExtractValue(backend->builder, pair, 1, "");
    if (!result || !overflow || !llvm_emit_trap_if(backend, state, overflow))
        return 0;

    *out_result = result;
    return 1;
}

static uint64_t signed_min_bits(unsigned bits)
{
    return UINT64_C(1) << (bits - 1);
}

static uint64_t signed_max_bits(unsigned bits)
{
    return (UINT64_C(1) << (bits - 1)) - UINT64_C(1);
}

static uint64_t unsigned_max_bits(unsigned bits)
{
    return bits == 64
        ? UINT64_MAX
        : (UINT64_C(1) << bits) - UINT64_C(1);
}

static int lower_checked_div_rem(
    LlvmBackend *backend,
    LlvmFunctionState *state,
    const CogIrType *type,
    CogIrOp op,
    LLVMValueRef lhs,
    LLVMValueRef rhs,
    LLVMValueRef *out_result
) {
    LLVMTypeRef llvm_type = LLVMTypeOf(lhs);
    LLVMValueRef zero = LLVMConstNull(llvm_type);
    LLVMValueRef divide_by_zero = LLVMBuildICmp(backend->builder, LLVMIntEQ, rhs, zero, "");
    if (!llvm_emit_trap_if(backend, state, divide_by_zero))
        return 0;

    if (type->as.integer.is_signed) {
        LLVMValueRef minimum = LLVMConstInt(llvm_type, signed_min_bits(type->as.integer.bits), 0);
        LLVMValueRef negative_one = LLVMConstInt(llvm_type, UINT64_MAX, 0);
        LLVMValueRef lhs_is_min = LLVMBuildICmp(backend->builder, LLVMIntEQ, lhs, minimum, "");
        LLVMValueRef rhs_is_negative_one = LLVMBuildICmp(backend->builder, LLVMIntEQ, rhs, negative_one, "");
        LLVMValueRef overflow = LLVMBuildAnd(backend->builder, lhs_is_min, rhs_is_negative_one, "");
        if (!llvm_emit_trap_if(backend, state, overflow))
            return 0;

        *out_result = op == COG_IR_OP_IDIV_CHECKED
            ? LLVMBuildSDiv(backend->builder, lhs, rhs, "")
            : LLVMBuildSRem(backend->builder, lhs, rhs, "");
    } else {
        *out_result = op == COG_IR_OP_IDIV_CHECKED
            ? LLVMBuildUDiv(backend->builder, lhs, rhs, "")
            : LLVMBuildURem(backend->builder, lhs, rhs, "");
    }

    return *out_result != NULL;
}

static int lower_checked_neg(
    LlvmBackend *backend,
    LlvmFunctionState *state,
    const CogIrType *type,
    LLVMValueRef operand,
    LLVMValueRef *out_result
) {
    LLVMTypeRef llvm_type = LLVMTypeOf(operand);
    LLVMValueRef zero = LLVMConstNull(llvm_type);
    LLVMValueRef invalid = NULL;
    if (type->as.integer.is_signed) {
        LLVMValueRef minimum = LLVMConstInt(llvm_type, signed_min_bits(type->as.integer.bits), 0);
        invalid = LLVMBuildICmp(backend->builder, LLVMIntEQ, operand, minimum, "");
    } else {
        invalid = LLVMBuildICmp(backend->builder, LLVMIntNE, operand, zero, "");
    }
    if (!llvm_emit_trap_if(backend, state, invalid))
        return 0;

    *out_result = LLVMBuildSub(backend->builder, zero, operand, "");
    return *out_result != NULL;
}

static LLVMValueRef normalize_shift_count(
    LlvmBackend *backend,
    const CogIrType *value_type,
    const CogIrType *count_type,
    LLVMValueRef count
) {
    LLVMTypeRef target = LLVMIntTypeInContext(backend->context, value_type->as.integer.bits);
    if (count_type->as.integer.bits < value_type->as.integer.bits)
        return LLVMBuildZExt(backend->builder, count, target, "");
    if (count_type->as.integer.bits > value_type->as.integer.bits)
        return LLVMBuildTrunc(backend->builder, count, target, "");
    return count;
}

static int lower_checked_shift(
    LlvmBackend *backend,
    LlvmFunctionState *state,
    const CogIrFunction *function,
    const CogIrInstruction *instruction,
    LLVMValueRef lhs,
    LLVMValueRef rhs,
    LLVMValueRef *out_result
) {
    const CogIrType *value_type = integer_runtime_type_for_value(
        backend,
        function,
        instruction->as.binary.lhs
    );
    const CogIrType *count_type = integer_runtime_type_for_value(
        backend,
        function,
        instruction->as.binary.rhs
    );
    if (!value_type || !count_type) {
        llvm_backend_error(backend, "checked shift requires integer CogIR operands");
        return 0;
    }

    LLVMTypeRef count_llvm_type = LLVMTypeOf(rhs);
    LLVMValueRef zero = LLVMConstNull(count_llvm_type);
    LLVMValueRef width = LLVMConstInt(count_llvm_type, value_type->as.integer.bits, 0);
    LLVMValueRef invalid = NULL;
    if (count_type->as.integer.is_signed) {
        LLVMValueRef negative = LLVMBuildICmp(backend->builder, LLVMIntSLT, rhs, zero, "");
        LLVMValueRef too_wide = LLVMBuildICmp(backend->builder, LLVMIntSGE, rhs, width, "");
        invalid = LLVMBuildOr(backend->builder, negative, too_wide, "");
    } else {
        invalid = LLVMBuildICmp(backend->builder, LLVMIntUGE, rhs, width, "");
    }
    if (!llvm_emit_trap_if(backend, state, invalid))
        return 0;

    LLVMValueRef count = normalize_shift_count(backend, value_type, count_type, rhs);
    if (!count) {
        llvm_backend_error(backend, "could not normalize LLVM shift count");
        return 0;
    }

    switch (instruction->op) {
        case COG_IR_OP_SHL_CHECKED_COUNT:
            *out_result = LLVMBuildShl(backend->builder, lhs, count, "");
            break;
        case COG_IR_OP_SHR_SIGNED_CHECKED_COUNT:
            *out_result = LLVMBuildAShr(backend->builder, lhs, count, "");
            break;
        case COG_IR_OP_SHR_UNSIGNED_CHECKED_COUNT:
            *out_result = LLVMBuildLShr(backend->builder, lhs, count, "");
            break;
        default:
            llvm_backend_error(backend, "invalid checked shift operation");
            return 0;
    }
    return *out_result != NULL;
}

static int lower_checked_integer_cast(
    LlvmBackend *backend,
    LlvmFunctionState *state,
    const CogIrFunction *function,
    const CogIrInstruction *instruction,
    LLVMValueRef operand,
    LLVMValueRef *out_result
) {
    const CogIrType *source = integer_runtime_type_for_value(
        backend,
        function,
        instruction->as.conversion.operand
    );
    const CogIrType *target = cog_ir_get_type(backend->ir, instruction->result_type);
    if (!source || !target || target->kind != COG_IR_TYPE_INTEGER) {
        llvm_backend_error(backend, "integer cast lowering requires integer-backed source and integer target");
        return 0;
    }

    unsigned source_bits = source->as.integer.bits;
    unsigned target_bits = target->as.integer.bits;
    LLVMTypeRef source_type = LLVMTypeOf(operand);
    LLVMValueRef invalid = NULL;

    if (source->as.integer.is_signed) {
        if (target->as.integer.is_signed) {
            if (target_bits < source_bits) {
                LLVMValueRef minimum = LLVMConstInt(source_type, ~signed_max_bits(target_bits), 0);
                LLVMValueRef maximum = LLVMConstInt(source_type, signed_max_bits(target_bits), 0);
                invalid = build_or_condition(
                    backend,
                    LLVMBuildICmp(backend->builder, LLVMIntSLT, operand, minimum, ""),
                    LLVMBuildICmp(backend->builder, LLVMIntSGT, operand, maximum, "")
                );
            }
        } else {
            invalid = LLVMBuildICmp(
                backend->builder,
                LLVMIntSLT,
                operand,
                LLVMConstNull(source_type),
                ""
            );
            if (target_bits < source_bits) {
                LLVMValueRef maximum = LLVMConstInt(source_type, unsigned_max_bits(target_bits), 0);
                invalid = build_or_condition(
                    backend,
                    invalid,
                    LLVMBuildICmp(backend->builder, LLVMIntSGT, operand, maximum, "")
                );
            }
        }
    } else if (target->as.integer.is_signed) {
        if (target_bits <= source_bits) {
            LLVMValueRef maximum = LLVMConstInt(source_type, signed_max_bits(target_bits), 0);
            invalid = LLVMBuildICmp(backend->builder, LLVMIntUGT, operand, maximum, "");
        }
    } else if (target_bits < source_bits) {
        LLVMValueRef maximum = LLVMConstInt(source_type, unsigned_max_bits(target_bits), 0);
        invalid = LLVMBuildICmp(backend->builder, LLVMIntUGT, operand, maximum, "");
    }

    if (invalid && !llvm_emit_trap_if(backend, state, invalid))
        return 0;

    LLVMTypeRef target_type = LLVMIntTypeInContext(backend->context, target_bits);
    if (target_bits < source_bits) {
        *out_result = LLVMBuildTrunc(backend->builder, operand, target_type, "");
    } else if (target_bits > source_bits) {
        *out_result = source->as.integer.is_signed && target->as.integer.is_signed
            ? LLVMBuildSExt(backend->builder, operand, target_type, "")
            : LLVMBuildZExt(backend->builder, operand, target_type, "");
    } else {
        *out_result = operand;
    }
    return *out_result != NULL;
}

static int lower_truncating_integer_cast(
    LlvmBackend *backend,
    const CogIrFunction *function,
    const CogIrInstruction *instruction,
    LLVMValueRef operand,
    LLVMValueRef *out_result
) {
    const CogIrType *source = integer_runtime_type_for_value(
        backend,
        function,
        instruction->as.conversion.operand
    );
    const CogIrType *target = cog_ir_get_type(backend->ir, instruction->result_type);
    if (!source || !target || target->kind != COG_IR_TYPE_INTEGER) {
        llvm_backend_error(backend, "int.truncate requires integer CogIR source and target");
        return 0;
    }

    unsigned source_bits = source->as.integer.bits;
    unsigned target_bits = target->as.integer.bits;
    LLVMTypeRef target_type = LLVMIntTypeInContext(backend->context, target_bits);
    if (target_bits < source_bits) {
        *out_result = LLVMBuildTrunc(backend->builder, operand, target_type, "");
    } else if (target_bits > source_bits) {
        *out_result = source->as.integer.is_signed
            ? LLVMBuildSExt(backend->builder, operand, target_type, "")
            : LLVMBuildZExt(backend->builder, operand, target_type, "");
    } else {
        *out_result = operand;
    }
    return *out_result != NULL;
}

int llvm_lower_integer_instruction(
    LlvmBackend *backend,
    const CogIrFunction *function,
    LlvmFunctionState *state,
    const CogIrInstruction *instruction,
    LLVMValueRef *out_result
) {
    LLVMValueRef lhs = NULL;
    LLVMValueRef rhs = NULL;
    LLVMValueRef operand = NULL;
    const CogIrType *type = cog_ir_get_type(backend->ir, instruction->result_type);

    switch (instruction->op) {
        case COG_IR_OP_IADD_CHECKED:
        case COG_IR_OP_ISUB_CHECKED:
        case COG_IR_OP_IMUL_CHECKED:
        case COG_IR_OP_IDIV_CHECKED:
        case COG_IR_OP_IREM_CHECKED:
        case COG_IR_OP_IADD_WRAP:
        case COG_IR_OP_ISUB_WRAP:
        case COG_IR_OP_IMUL_WRAP:
        case COG_IR_OP_BIT_AND:
        case COG_IR_OP_BIT_OR:
        case COG_IR_OP_BIT_XOR:
        case COG_IR_OP_SHL_CHECKED_COUNT:
        case COG_IR_OP_SHR_SIGNED_CHECKED_COUNT:
        case COG_IR_OP_SHR_UNSIGNED_CHECKED_COUNT:
            lhs = state->values[instruction->as.binary.lhs];
            rhs = state->values[instruction->as.binary.rhs];
            if (!lhs || !rhs) {
                llvm_backend_error(backend, "integer operation references unavailable LLVM value");
                return 0;
            }
            break;
        case COG_IR_OP_INEG_CHECKED:
        case COG_IR_OP_INEG_WRAP:
        case COG_IR_OP_BIT_NOT:
            operand = state->values[instruction->as.unary.operand];
            if (!operand) {
                llvm_backend_error(backend, "integer unary operation references unavailable LLVM value");
                return 0;
            }
            break;
        case COG_IR_OP_CAST_CHECKED:
        case COG_IR_OP_INT_TRUNCATE:
            operand = state->values[instruction->as.conversion.operand];
            if (!operand) {
                llvm_backend_error(backend, "integer conversion references unavailable LLVM value");
                return 0;
            }
            break;
        default:
            llvm_backend_error(backend, "invalid integer operation dispatch");
            return 0;
    }

    if (instruction->op != COG_IR_OP_CAST_CHECKED &&
        instruction->op != COG_IR_OP_INT_TRUNCATE &&
        (!type || type->kind != COG_IR_TYPE_INTEGER)) {
        llvm_backend_error(backend, "integer operation has non-integer CogIR result type");
        return 0;
    }

    switch (instruction->op) {
        case COG_IR_OP_IADD_CHECKED:
        case COG_IR_OP_ISUB_CHECKED:
        case COG_IR_OP_IMUL_CHECKED:
            return lower_overflow_checked_binary(
                backend, state, type, instruction->op, lhs, rhs, out_result
            );
        case COG_IR_OP_IDIV_CHECKED:
        case COG_IR_OP_IREM_CHECKED:
            return lower_checked_div_rem(
                backend, state, type, instruction->op, lhs, rhs, out_result
            );
        case COG_IR_OP_INEG_CHECKED:
            return lower_checked_neg(backend, state, type, operand, out_result);

        case COG_IR_OP_IADD_WRAP:
            *out_result = LLVMBuildAdd(backend->builder, lhs, rhs, "");
            return *out_result != NULL;
        case COG_IR_OP_ISUB_WRAP:
            *out_result = LLVMBuildSub(backend->builder, lhs, rhs, "");
            return *out_result != NULL;
        case COG_IR_OP_IMUL_WRAP:
            *out_result = LLVMBuildMul(backend->builder, lhs, rhs, "");
            return *out_result != NULL;
        case COG_IR_OP_INEG_WRAP:
            *out_result = LLVMBuildSub(
                backend->builder,
                LLVMConstNull(LLVMTypeOf(operand)),
                operand,
                ""
            );
            return *out_result != NULL;

        case COG_IR_OP_BIT_AND:
            *out_result = LLVMBuildAnd(backend->builder, lhs, rhs, "");
            return *out_result != NULL;
        case COG_IR_OP_BIT_OR:
            *out_result = LLVMBuildOr(backend->builder, lhs, rhs, "");
            return *out_result != NULL;
        case COG_IR_OP_BIT_XOR:
            *out_result = LLVMBuildXor(backend->builder, lhs, rhs, "");
            return *out_result != NULL;
        case COG_IR_OP_BIT_NOT:
            *out_result = LLVMBuildNot(backend->builder, operand, "");
            return *out_result != NULL;

        case COG_IR_OP_SHL_CHECKED_COUNT:
        case COG_IR_OP_SHR_SIGNED_CHECKED_COUNT:
        case COG_IR_OP_SHR_UNSIGNED_CHECKED_COUNT:
            return lower_checked_shift(
                backend, state, function, instruction, lhs, rhs, out_result
            );

        case COG_IR_OP_CAST_CHECKED:
            return lower_checked_integer_cast(
                backend, state, function, instruction, operand, out_result
            );
        case COG_IR_OP_INT_TRUNCATE:
            return lower_truncating_integer_cast(
                backend, function, instruction, operand, out_result
            );

        default:
            llvm_backend_error(backend, "invalid integer operation dispatch");
            return 0;
    }
}
