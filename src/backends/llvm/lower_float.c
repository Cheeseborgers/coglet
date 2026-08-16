#include "backend_llvm_internal.h"

#include <float.h>
#include <stdint.h>


static double power_of_two(unsigned exponent)
{
    double value = 1.0;
    for (unsigned i = 0; i < exponent; ++i)
        value *= 2.0;
    return value;
}

static const CogIrType *value_type(
    LlvmBackend *backend,
    const CogIrFunction *function,
    CogIrValueId value_id
) {
    const CogIrValue *value = cog_ir_get_value(function, value_id);
    return value ? cog_ir_get_type(backend->ir, value->type) : NULL;
}

static LLVMValueRef float_bits_constant(
    LlvmBackend *backend,
    unsigned bits,
    uint64_t value
) {
    LLVMTypeRef integer = LLVMIntTypeInContext(backend->context, bits);
    LLVMTypeRef floating = bits == 32
        ? LLVMFloatTypeInContext(backend->context)
        : LLVMDoubleTypeInContext(backend->context);
    return LLVMConstBitCast(LLVMConstInt(integer, value, 0), floating);
}

static LLVMValueRef positive_infinity(LlvmBackend *backend, unsigned bits)
{
    return bits == 32
        ? float_bits_constant(backend, 32, UINT32_C(0x7f800000))
        : float_bits_constant(backend, 64, UINT64_C(0x7ff0000000000000));
}

static LLVMValueRef negative_infinity(LlvmBackend *backend, unsigned bits)
{
    return bits == 32
        ? float_bits_constant(backend, 32, UINT32_C(0xff800000))
        : float_bits_constant(backend, 64, UINT64_C(0xfff0000000000000));
}

static LLVMRealPredicate predicate_for_float_compare(CogIrOp op)
{
    switch (op) {
        case COG_IR_OP_FCMP_EQ: return LLVMRealOEQ;
        case COG_IR_OP_FCMP_NE: return LLVMRealUNE;
        case COG_IR_OP_FCMP_LT: return LLVMRealOLT;
        case COG_IR_OP_FCMP_LE: return LLVMRealOLE;
        case COG_IR_OP_FCMP_GT: return LLVMRealOGT;
        case COG_IR_OP_FCMP_GE: return LLVMRealOGE;
        default: return LLVMRealPredicateFalse;
    }
}

static int lower_float_to_integer(
    LlvmBackend *backend,
    LlvmFunctionState *state,
    const CogIrType *source,
    const CogIrType *target,
    LLVMValueRef operand,
    LLVMValueRef *out_result
) {
    LLVMValueRef checked_operand = operand;
    LLVMTypeRef source_type = LLVMTypeOf(operand);
    if (source->as.floating.bits == 32) {
        source_type = LLVMDoubleTypeInContext(backend->context);
        checked_operand = LLVMBuildFPExt(backend->builder, operand, source_type, "");
        if (!checked_operand) {
            llvm_backend_error(backend, "could not widen f32 for checked integer conversion");
            return 0;
        }
    }

    LLVMValueRef invalid = LLVMBuildFCmp(
        backend->builder,
        LLVMRealUNO,
        checked_operand,
        checked_operand,
        ""
    );

    unsigned target_bits = target->as.integer.bits;
    if (target->as.integer.is_signed) {
        double upper = power_of_two(target_bits - 1);
        LLVMValueRef upper_bound = LLVMConstReal(source_type, upper);
        LLVMValueRef too_high = LLVMBuildFCmp(
            backend->builder,
            LLVMRealOGE,
            checked_operand,
            upper_bound,
            ""
        );
        invalid = LLVMBuildOr(backend->builder, invalid, too_high, "");

        if (target_bits == 64) {
            LLVMValueRef lower_bound = LLVMConstReal(source_type, -power_of_two(63));
            LLVMValueRef too_low = LLVMBuildFCmp(
                backend->builder,
                LLVMRealOLT,
                checked_operand,
                lower_bound,
                ""
            );
            invalid = LLVMBuildOr(backend->builder, invalid, too_low, "");
        } else {
            double lower_invalid = -(upper + 1.0);
            LLVMValueRef lower_bound = LLVMConstReal(source_type, lower_invalid);
            LLVMValueRef too_low = LLVMBuildFCmp(
                backend->builder,
                LLVMRealOLE,
                checked_operand,
                lower_bound,
                ""
            );
            invalid = LLVMBuildOr(backend->builder, invalid, too_low, "");
        }
    } else {
        LLVMValueRef negative_limit = LLVMConstReal(source_type, -1.0);
        LLVMValueRef too_low = LLVMBuildFCmp(
            backend->builder,
            LLVMRealOLE,
            checked_operand,
            negative_limit,
            ""
        );
        LLVMValueRef upper_bound = LLVMConstReal(
            source_type,
            power_of_two(target_bits)
        );
        LLVMValueRef too_high = LLVMBuildFCmp(
            backend->builder,
            LLVMRealOGE,
            checked_operand,
            upper_bound,
            ""
        );
        invalid = LLVMBuildOr(backend->builder, invalid, too_low, "");
        invalid = LLVMBuildOr(backend->builder, invalid, too_high, "");
    }

    if (!llvm_emit_trap_if(backend, state, invalid))
        return 0;

    LLVMTypeRef target_type = LLVMIntTypeInContext(backend->context, target_bits);
    *out_result = target->as.integer.is_signed
        ? LLVMBuildFPToSI(backend->builder, operand, target_type, "")
        : LLVMBuildFPToUI(backend->builder, operand, target_type, "");
    return *out_result != NULL;
}

static int lower_float_to_float(
    LlvmBackend *backend,
    LlvmFunctionState *state,
    const CogIrType *source,
    const CogIrType *target,
    LLVMValueRef operand,
    LLVMValueRef *out_result
) {
    if (source->as.floating.bits == target->as.floating.bits) {
        *out_result = operand;
        return 1;
    }

    LLVMTypeRef target_type = target->as.floating.bits == 32
        ? LLVMFloatTypeInContext(backend->context)
        : LLVMDoubleTypeInContext(backend->context);

    if (source->as.floating.bits == 32 && target->as.floating.bits == 64) {
        *out_result = LLVMBuildFPExt(backend->builder, operand, target_type, "");
        return *out_result != NULL;
    }

    if (source->as.floating.bits == 64 && target->as.floating.bits == 32) {
        LLVMTypeRef source_type = LLVMTypeOf(operand);
        LLVMValueRef maximum = LLVMConstReal(source_type, (double)FLT_MAX);
        LLVMValueRef minimum = LLVMConstReal(source_type, -(double)FLT_MAX);
        LLVMValueRef above = LLVMBuildFCmp(
            backend->builder, LLVMRealOGT, operand, maximum, ""
        );
        LLVMValueRef below = LLVMBuildFCmp(
            backend->builder, LLVMRealOLT, operand, minimum, ""
        );
        LLVMValueRef outside = LLVMBuildOr(backend->builder, above, below, "");

        LLVMValueRef pos_inf = positive_infinity(backend, 64);
        LLVMValueRef neg_inf = negative_infinity(backend, 64);
        LLVMValueRef is_pos_inf = LLVMBuildFCmp(
            backend->builder, LLVMRealOEQ, operand, pos_inf, ""
        );
        LLVMValueRef is_neg_inf = LLVMBuildFCmp(
            backend->builder, LLVMRealOEQ, operand, neg_inf, ""
        );
        LLVMValueRef is_inf = LLVMBuildOr(backend->builder, is_pos_inf, is_neg_inf, "");
        LLVMValueRef not_inf = LLVMBuildNot(backend->builder, is_inf, "");
        LLVMValueRef invalid = LLVMBuildAnd(backend->builder, outside, not_inf, "");
        if (!llvm_emit_trap_if(backend, state, invalid))
            return 0;

        *out_result = LLVMBuildFPTrunc(backend->builder, operand, target_type, "");
        return *out_result != NULL;
    }

    llvm_backend_error(backend, "unsupported checked floating-point width conversion");
    return 0;
}

static int lower_checked_numeric_cast(
    LlvmBackend *backend,
    const CogIrFunction *function,
    LlvmFunctionState *state,
    const CogIrInstruction *instruction,
    LLVMValueRef *out_result
) {
    LLVMValueRef operand = state->values[instruction->as.conversion.operand];
    const CogIrType *source = value_type(
        backend, function, instruction->as.conversion.operand
    );
    const CogIrType *target = cog_ir_get_type(backend->ir, instruction->result_type);
    if (!operand || !source || !target) {
        llvm_backend_error(backend, "floating checked cast references unavailable value or type");
        return 0;
    }

    if (source->kind == COG_IR_TYPE_FLOAT && target->kind == COG_IR_TYPE_INTEGER)
        return lower_float_to_integer(backend, state, source, target, operand, out_result);

    if (source->kind == COG_IR_TYPE_INTEGER && target->kind == COG_IR_TYPE_FLOAT) {
        LLVMTypeRef target_type = llvm_lower_type(backend, instruction->result_type);
        if (!target_type)
            return 0;
        *out_result = source->as.integer.is_signed
            ? LLVMBuildSIToFP(backend->builder, operand, target_type, "")
            : LLVMBuildUIToFP(backend->builder, operand, target_type, "");
        return *out_result != NULL;
    }

    if (source->kind == COG_IR_TYPE_FLOAT && target->kind == COG_IR_TYPE_FLOAT)
        return lower_float_to_float(backend, state, source, target, operand, out_result);

    llvm_backend_error(backend, "invalid floating checked-cast dispatch");
    return 0;
}

int llvm_lower_float_instruction(
    LlvmBackend *backend,
    const CogIrFunction *function,
    LlvmFunctionState *state,
    const CogIrInstruction *instruction,
    LLVMValueRef *out_result
) {
    LLVMValueRef lhs = NULL;
    LLVMValueRef rhs = NULL;
    LLVMValueRef operand = NULL;

    switch (instruction->op) {
        case COG_IR_OP_FADD:
        case COG_IR_OP_FSUB:
        case COG_IR_OP_FMUL:
        case COG_IR_OP_FDIV:
        case COG_IR_OP_FCMP_EQ:
        case COG_IR_OP_FCMP_NE:
        case COG_IR_OP_FCMP_LT:
        case COG_IR_OP_FCMP_LE:
        case COG_IR_OP_FCMP_GT:
        case COG_IR_OP_FCMP_GE:
            lhs = state->values[instruction->as.binary.lhs];
            rhs = state->values[instruction->as.binary.rhs];
            if (!lhs || !rhs) {
                llvm_backend_error(backend, "floating operation references unavailable LLVM value");
                return 0;
            }
            break;
        case COG_IR_OP_FNEG:
            operand = state->values[instruction->as.unary.operand];
            if (!operand) {
                llvm_backend_error(backend, "floating negation references unavailable LLVM value");
                return 0;
            }
            break;
        case COG_IR_OP_CAST_CHECKED:
            return lower_checked_numeric_cast(
                backend, function, state, instruction, out_result
            );
        default:
            llvm_backend_error(backend, "invalid floating operation dispatch");
            return 0;
    }

    switch (instruction->op) {
        case COG_IR_OP_FADD:
            *out_result = LLVMBuildFAdd(backend->builder, lhs, rhs, "");
            break;
        case COG_IR_OP_FSUB:
            *out_result = LLVMBuildFSub(backend->builder, lhs, rhs, "");
            break;
        case COG_IR_OP_FMUL:
            *out_result = LLVMBuildFMul(backend->builder, lhs, rhs, "");
            break;
        case COG_IR_OP_FDIV:
            *out_result = LLVMBuildFDiv(backend->builder, lhs, rhs, "");
            break;
        case COG_IR_OP_FNEG:
            *out_result = LLVMBuildFNeg(backend->builder, operand, "");
            break;
        case COG_IR_OP_FCMP_EQ:
        case COG_IR_OP_FCMP_NE:
        case COG_IR_OP_FCMP_LT:
        case COG_IR_OP_FCMP_LE:
        case COG_IR_OP_FCMP_GT:
        case COG_IR_OP_FCMP_GE:
            *out_result = LLVMBuildFCmp(
                backend->builder,
                predicate_for_float_compare(instruction->op),
                lhs,
                rhs,
                ""
            );
            break;
        default:
            llvm_backend_error(backend, "invalid floating operation dispatch");
            return 0;
    }

    return *out_result != NULL;
}
