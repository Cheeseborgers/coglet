#include "ir/cog_ir.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void ir_error(DiagnosticList *diagnostics, SourceSpan span, const char *fmt, ...)
{
    if (!diagnostics)
        return;

    char buffer[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    diagnostic_add(
        diagnostics,
        DIAGNOSTIC_ERROR,
        DIAGNOSTIC_PHASE_IR,
        span,
        buffer
    );
}

static int valid_span(const CogIrModule *module, SourceSpan span)
{
    if (!source_span_is_valid(span))
        return span.file_id == SOURCE_FILE_ID_INVALID;

    const SourceFile *file = source_manager_get(&module->sources, span.file_id);
    if (!file)
        return 0;

    return span.start_offset <= span.end_offset && span.end_offset <= file->length;
}

static int optional_abi_matches_runtime(
    const CogIrModule *module,
    CogIrAbiTypeId abi_type,
    CogIrTypeId runtime_type
) {
    if (abi_type == COG_IR_ABI_TYPE_INVALID)
        return 1;
    const CogIrAbiType *abi = cog_ir_get_abi_type(module, abi_type);
    return abi && abi->runtime_type == runtime_type;
}

static int is_integer_type(const CogIrModule *module, CogIrTypeId id)
{
    const CogIrType *type = cog_ir_get_type(module, id);
    return type && type->kind == COG_IR_TYPE_INTEGER;
}

static int is_float_type(const CogIrModule *module, CogIrTypeId id)
{
    const CogIrType *type = cog_ir_get_type(module, id);
    return type && type->kind == COG_IR_TYPE_FLOAT;
}

static int is_pointer_comparable_type(const CogIrModule *module, CogIrTypeId id)
{
    const CogIrType *type = cog_ir_get_type(module, id);
    return type && (type->kind == COG_IR_TYPE_POINTER ||
                    type->kind == COG_IR_TYPE_OPAQUE_POINTER ||
                    type->kind == COG_IR_TYPE_FUNCTION);
}

static int pointer_pointee(
    const CogIrModule *module,
    CogIrTypeId pointer_type,
    CogIrTypeId *out,
    int *readonly,
    int *volatile_access
) {
    const CogIrType *type = cog_ir_get_type(module, pointer_type);
    if (!type || type->kind != COG_IR_TYPE_POINTER)
        return 0;

    if (out) *out = type->as.pointer.pointee;
    if (readonly) *readonly = type->as.pointer.is_readonly;
    if (volatile_access) *volatile_access = type->as.pointer.is_volatile;
    return 1;
}

static unsigned integer_width(const CogIrModule *module, CogIrTypeId id)
{
    const CogIrType *type = cog_ir_get_type(module, id);
    if (!type)
        return 0;
    if (type->kind == COG_IR_TYPE_INTEGER)
        return type->as.integer.bits;
    if (type->kind == COG_IR_TYPE_ENUM) {
        const CogIrType *backing = cog_ir_get_type(module, type->as.enumeration.backing_type);
        return backing && backing->kind == COG_IR_TYPE_INTEGER ? backing->as.integer.bits : 0;
    }
    return 0;
}

static int is_native_c_int_type(const CogIrModule *module, const CogIrType *type)
{
    return type && type->kind == COG_IR_TYPE_INTEGER &&
           type->as.integer.bits == module->target.c_int_bits &&
           type->as.integer.is_signed;
}

static int c_vararg_promotion_valid(
    const CogIrModule *module,
    const CogIrType *source,
    const CogIrType *target
)
{
    if (!source || !target || source->id == target->id)
        return 0;

    if (source->kind == COG_IR_TYPE_BOOL)
        return is_native_c_int_type(module, target);

    if (source->kind == COG_IR_TYPE_FLOAT)
        return source->as.floating.bits == 32 &&
               target->kind == COG_IR_TYPE_FLOAT &&
               target->as.floating.bits == 64;

    const CogIrType *integer = source;
    if (source->kind == COG_IR_TYPE_ENUM) {
        if (!source->as.enumeration.is_repr_c)
            return 0;
        integer = cog_ir_get_type(module, source->as.enumeration.backing_type);
    }

    return integer && integer->kind == COG_IR_TYPE_INTEGER &&
           integer->as.integer.bits < module->target.c_int_bits &&
           is_native_c_int_type(module, target);
}

static int c_vararg_argument_type_is_legal(
    const CogIrModule *module,
    CogIrTypeId type_id
)
{
    const CogIrType *type = cog_ir_get_type(module, type_id);
    if (!type)
        return 0;

    switch (type->kind) {
        case COG_IR_TYPE_INTEGER:
            return type->as.integer.bits >= module->target.c_int_bits;

        case COG_IR_TYPE_FLOAT:
            return type->as.floating.bits == 64;

        case COG_IR_TYPE_POINTER:
        case COG_IR_TYPE_OPAQUE_POINTER:
            return 1;

        case COG_IR_TYPE_ENUM: {
            if (!type->as.enumeration.is_repr_c)
                return 0;
            const CogIrType *backing =
                cog_ir_get_type(module, type->as.enumeration.backing_type);
            return backing && backing->kind == COG_IR_TYPE_INTEGER &&
                   backing->as.integer.bits >= module->target.c_int_bits;
        }

        case COG_IR_TYPE_FUNCTION:
            return type->as.function.abi == COG_IR_ABI_C;

        case COG_IR_TYPE_VOID:
        case COG_IR_TYPE_BOOL:
        case COG_IR_TYPE_ARRAY:
        case COG_IR_TYPE_STRUCT:
        case COG_IR_TYPE_UNION:
            return 0;
    }

    return 0;
}


static int type_has_runtime_object_layout(const CogIrType *type)
{
    if (!type)
        return 0;

    switch (type->kind) {
        case COG_IR_TYPE_VOID:
            return 0;
        case COG_IR_TYPE_FUNCTION:
            return type->as.function.abi == COG_IR_ABI_C;
        case COG_IR_TYPE_STRUCT:
        case COG_IR_TYPE_UNION:
            return type->as.aggregate.is_complete &&
                   !type->as.aggregate.is_incomplete;
        case COG_IR_TYPE_BOOL:
        case COG_IR_TYPE_INTEGER:
        case COG_IR_TYPE_FLOAT:
        case COG_IR_TYPE_POINTER:
        case COG_IR_TYPE_OPAQUE_POINTER:
        case COG_IR_TYPE_ARRAY:
        case COG_IR_TYPE_ENUM:
            return 1;
    }

    return 0;
}

static int constant_matches_type(
    const CogIrModule *module,
    const CogIrConstant *constant,
    DiagnosticList *diagnostics
) {
    const CogIrType *type = cog_ir_get_type(module, constant->type);
    if (!type) {
        ir_error(diagnostics, source_span_invalid(), "constant @c%u has invalid type", constant->id);
        return 0;
    }

    switch (constant->kind) {
        case COG_IR_CONST_ZERO:
            /* cfn(...) values are first-class storable function pointers. */
            if (type->kind == COG_IR_TYPE_VOID) {
                ir_error(diagnostics, type->span, "zero constant @c%u has non-storable type", constant->id);
                return 0;
            }
            return 1;

        case COG_IR_CONST_BOOL:
            if (type->kind != COG_IR_TYPE_BOOL) {
                ir_error(diagnostics, type->span, "bool constant @c%u does not have bool type", constant->id);
                return 0;
            }
            return 1;

        case COG_IR_CONST_INTEGER: {
            unsigned bits = integer_width(module, constant->type);
            if (!bits) {
                ir_error(diagnostics, type->span, "integer constant @c%u does not have integer/enum type", constant->id);
                return 0;
            }
            if (bits < 64 && (constant->as.integer_bits >> bits) != 0) {
                ir_error(diagnostics, type->span, "integer constant @c%u has bits outside its %u-bit type", constant->id, bits);
                return 0;
            }
            return 1;
        }

        case COG_IR_CONST_FLOAT32:
            if (type->kind != COG_IR_TYPE_FLOAT || type->as.floating.bits != 32) {
                ir_error(diagnostics, type->span, "f32 constant @c%u has wrong type", constant->id);
                return 0;
            }
            return 1;

        case COG_IR_CONST_FLOAT64:
            if (type->kind != COG_IR_TYPE_FLOAT || type->as.floating.bits != 64) {
                ir_error(diagnostics, type->span, "f64 constant @c%u has wrong type", constant->id);
                return 0;
            }
            return 1;

        case COG_IR_CONST_NULL:
            if (type->kind != COG_IR_TYPE_POINTER &&
                type->kind != COG_IR_TYPE_OPAQUE_POINTER &&
                type->kind != COG_IR_TYPE_FUNCTION) {
                ir_error(diagnostics, type->span, "null constant @c%u has non-nullable type", constant->id);
                return 0;
            }
            return 1;

        case COG_IR_CONST_ARRAY:
            if (type->kind != COG_IR_TYPE_ARRAY ||
                constant->as.aggregate.element_count != type->as.array.length) {
                ir_error(diagnostics, type->span, "array constant @c%u has wrong element count/type", constant->id);
                return 0;
            }
            for (size_t i = 0; i < constant->as.aggregate.element_count; ++i) {
                const CogIrConstant *element = cog_ir_get_constant(module, constant->as.aggregate.elements[i]);
                if (!element || element->type != type->as.array.element_type) {
                    ir_error(diagnostics, type->span, "array constant @c%u element %zu has wrong type", constant->id, i);
                    return 0;
                }
            }
            return 1;

        case COG_IR_CONST_STRUCT:
            if (type->kind != COG_IR_TYPE_STRUCT || !type->as.aggregate.is_complete ||
                constant->as.aggregate.element_count != type->as.aggregate.field_count) {
                ir_error(diagnostics, type->span, "struct constant @c%u has wrong field count/type", constant->id);
                return 0;
            }
            for (size_t i = 0; i < constant->as.aggregate.element_count; ++i) {
                const CogIrConstant *field = cog_ir_get_constant(module, constant->as.aggregate.elements[i]);
                if (!field || field->type != type->as.aggregate.fields[i].type) {
                    ir_error(diagnostics, type->span, "struct constant @c%u field %zu has wrong type", constant->id, i);
                    return 0;
                }
            }
            return 1;
    }

    return 0;
}

static int value_available(
    const CogIrFunction *function,
    CogIrBlockId block,
    size_t instruction_ordinal,
    CogIrValueId value_id
) {
    const CogIrValue *value = cog_ir_get_value(function, value_id);
    if (!value)
        return 0;

    switch (value->kind) {
        case COG_IR_VALUE_FUNCTION_PARAMETER:
            return 1;
        case COG_IR_VALUE_BLOCK_PARAMETER:
            return value->block == block;
        case COG_IR_VALUE_INSTRUCTION: {
            if (value->block != block || value->ordinal >= instruction_ordinal ||
                (size_t)value->block >= function->block_count)
                return 0;
            const CogIrBlock *producer_block = &function->blocks[value->block];
            if (value->ordinal >= producer_block->instruction_count)
                return 0;
            return !producer_block->instructions[value->ordinal].result_is_discarded;
        }
    }
    return 0;
}

static int terminator_value_available(
    const CogIrFunction *function,
    CogIrBlockId block,
    CogIrValueId value_id
) {
    return value_available(function, block, (size_t)-1, value_id);
}

static int verify_edge(
    const CogIrModule *module,
    const CogIrFunction *function,
    CogIrBlockId from,
    const CogIrBranchEdge *edge,
    SourceSpan span,
    DiagnosticList *diagnostics
) {
    (void)module;
    const CogIrBlock *target = cog_ir_get_block(function, edge->target);
    if (!target) {
        ir_error(diagnostics, span, "branch from block %u targets invalid block %u", from, edge->target);
        return 0;
    }
    if (edge->argument_count != target->parameter_count) {
        ir_error(diagnostics, span, "branch to block %u passes %zu arguments; expected %zu",
                 edge->target, edge->argument_count, target->parameter_count);
        return 0;
    }
    for (size_t i = 0; i < edge->argument_count; ++i) {
        const CogIrValue *value = cog_ir_get_value(function, edge->arguments[i]);
        if (!value || !terminator_value_available(function, from, edge->arguments[i]) ||
            value->type != target->parameters[i].type) {
            ir_error(diagnostics, span, "branch argument %zu to block %u is invalid or has wrong type", i, edge->target);
            return 0;
        }
    }
    return 1;
}

static int verify_instruction(
    const CogIrModule *module,
    const CogIrFunction *function,
    const CogIrBlock *block,
    size_t ordinal,
    const CogIrInstruction *instruction,
    DiagnosticList *diagnostics
) {
    int ok = 1;
    const CogIrValue *result = instruction->result == COG_IR_VALUE_INVALID
        ? NULL : cog_ir_get_value(function, instruction->result);

    if (!valid_span(module, instruction->span)) {
        ir_error(diagnostics, source_span_invalid(), "instruction %zu in block %u has invalid source span", ordinal, block->id);
        ok = 0;
    }

    if (instruction->result_type == COG_IR_TYPE_INVALID) {
        if (instruction->result != COG_IR_VALUE_INVALID) {
            ir_error(diagnostics, instruction->span, "void instruction %zu in block %u has a result value", ordinal, block->id);
            ok = 0;
        }
        if (instruction->result_is_discarded) {
            ir_error(diagnostics, instruction->span, "void instruction %zu in block %u cannot discard a result", ordinal, block->id);
            ok = 0;
        }
    } else if (!result || result->type != instruction->result_type ||
               result->kind != COG_IR_VALUE_INSTRUCTION || result->block != block->id || result->ordinal != ordinal) {
        ir_error(diagnostics, instruction->span, "instruction %zu in block %u has inconsistent result metadata", ordinal, block->id);
        ok = 0;
    }

#define REQUIRE_VALUE(v, label) do { \
    if (!value_available(function, block->id, ordinal, (v))) { \
        ir_error(diagnostics, instruction->span, "%s operand of %s in block %u is not available", (label), cog_ir_op_name(instruction->op), block->id); \
        ok = 0; \
    } \
} while (0)

    switch (instruction->op) {
        case COG_IR_OP_CONST: {
            const CogIrConstant *constant = cog_ir_get_constant(module, instruction->as.constant.constant);
            if (!constant || instruction->result_type == COG_IR_TYPE_INVALID || constant->type != instruction->result_type) {
                ir_error(diagnostics, instruction->span, "const instruction has invalid constant/result type");
                ok = 0;
            }
            break;
        }

        case COG_IR_OP_FUNCTION_REF: {
            const CogIrFunction *target = cog_ir_get_function(module, instruction->as.function_ref.function);
            if (!target || instruction->result_type != target->type) {
                ir_error(diagnostics, instruction->span, "function_ref has invalid function/result type");
                ok = 0;
            }
            break;
        }

        case COG_IR_OP_SIZE_OF:
        case COG_IR_OP_ALIGN_OF: {
            const CogIrType *result_type = cog_ir_get_type(module, instruction->result_type);
            const CogIrType *queried = cog_ir_get_type(module, instruction->as.type_query.queried_type);
            if (!type_has_runtime_object_layout(queried) || !result_type ||
                result_type->kind != COG_IR_TYPE_INTEGER ||
                result_type->as.integer.bits != 64 || result_type->as.integer.is_signed) {
                ir_error(diagnostics, instruction->span,
                    "type-layout query requires a layout-capable queried type and u64 result");
                ok = 0;
            }
            break;
        }

        case COG_IR_OP_LOCAL_ADDR: {
            const CogIrSlot *slot = cog_ir_get_slot(function, instruction->as.local_addr.slot);
            CogIrTypeId pointee = COG_IR_TYPE_INVALID;
            int readonly = 0;
            if (!slot || !pointer_pointee(module, instruction->result_type, &pointee, &readonly, NULL) ||
                readonly || pointee != slot->type) {
                ir_error(diagnostics, instruction->span, "local_addr has invalid slot/result pointer type");
                ok = 0;
            }
            break;
        }

        case COG_IR_OP_GLOBAL_ADDR: {
            const CogIrGlobal *global = cog_ir_get_global(module, instruction->as.global_addr.global);
            CogIrTypeId pointee = COG_IR_TYPE_INVALID;
            int readonly = 0;
            if (!global || !pointer_pointee(module, instruction->result_type, &pointee, &readonly, NULL) ||
                pointee != global->type || (!!readonly != !!global->is_readonly)) {
                ir_error(diagnostics, instruction->span, "global_addr has invalid global/result pointer type");
                ok = 0;
            }
            break;
        }

        case COG_IR_OP_LOAD: {
            REQUIRE_VALUE(instruction->as.load.address, "address");
            const CogIrValue *address = cog_ir_get_value(function, instruction->as.load.address);
            CogIrTypeId pointee = COG_IR_TYPE_INVALID;
            int ptr_volatile = 0;
            if (!address || !pointer_pointee(module, address->type, &pointee, NULL, &ptr_volatile) ||
                pointee != instruction->result_type || (ptr_volatile && !instruction->as.load.is_volatile)) {
                ir_error(diagnostics, instruction->span, "load has invalid pointer/result or loses volatile semantics");
                ok = 0;
            }
            break;
        }

        case COG_IR_OP_STORE: {
            REQUIRE_VALUE(instruction->as.store.address, "address");
            REQUIRE_VALUE(instruction->as.store.value, "value");
            const CogIrValue *address = cog_ir_get_value(function, instruction->as.store.address);
            const CogIrValue *value = cog_ir_get_value(function, instruction->as.store.value);
            CogIrTypeId pointee = COG_IR_TYPE_INVALID;
            int readonly = 0;
            int ptr_volatile = 0;
            if (instruction->result_type != COG_IR_TYPE_INVALID || !address || !value ||
                !pointer_pointee(module, address->type, &pointee, &readonly, &ptr_volatile) ||
                readonly || pointee != value->type || (ptr_volatile && !instruction->as.store.is_volatile)) {
                ir_error(diagnostics, instruction->span, "store has invalid address/value, readonly target, or loses volatile semantics");
                ok = 0;
            }
            break;
        }

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
        case COG_IR_OP_BIT_XOR: {
            REQUIRE_VALUE(instruction->as.binary.lhs, "left");
            REQUIRE_VALUE(instruction->as.binary.rhs, "right");
            const CogIrValue *lhs = cog_ir_get_value(function, instruction->as.binary.lhs);
            const CogIrValue *rhs = cog_ir_get_value(function, instruction->as.binary.rhs);
            if (!lhs || !rhs || !is_integer_type(module, lhs->type) || lhs->type != rhs->type || lhs->type != instruction->result_type) {
                ir_error(diagnostics, instruction->span, "%s requires matching integer operands/result", cog_ir_op_name(instruction->op));
                ok = 0;
            }
            break;
        }

        case COG_IR_OP_SHL_CHECKED_COUNT:
        case COG_IR_OP_SHR_SIGNED_CHECKED_COUNT:
        case COG_IR_OP_SHR_UNSIGNED_CHECKED_COUNT: {
            REQUIRE_VALUE(instruction->as.binary.lhs, "left");
            REQUIRE_VALUE(instruction->as.binary.rhs, "right");
            const CogIrValue *lhs = cog_ir_get_value(function, instruction->as.binary.lhs);
            const CogIrValue *rhs = cog_ir_get_value(function, instruction->as.binary.rhs);
            const CogIrType *lhs_type = lhs ? cog_ir_get_type(module, lhs->type) : NULL;
            if (!lhs || !rhs || !lhs_type || lhs_type->kind != COG_IR_TYPE_INTEGER ||
                !is_integer_type(module, rhs->type) || lhs->type != instruction->result_type) {
                ir_error(diagnostics, instruction->span, "%s requires integer operands and a result matching the left operand", cog_ir_op_name(instruction->op));
                ok = 0;
                break;
            }
            if (instruction->op == COG_IR_OP_SHR_SIGNED_CHECKED_COUNT && !lhs_type->as.integer.is_signed) {
                ir_error(diagnostics, instruction->span, "signed right shift requires signed left operand");
                ok = 0;
            }
            if (instruction->op == COG_IR_OP_SHR_UNSIGNED_CHECKED_COUNT && lhs_type->as.integer.is_signed) {
                ir_error(diagnostics, instruction->span, "unsigned right shift requires unsigned left operand");
                ok = 0;
            }
            break;
        }

        case COG_IR_OP_INEG_CHECKED:
        case COG_IR_OP_INEG_WRAP:
        case COG_IR_OP_BIT_NOT: {
            REQUIRE_VALUE(instruction->as.unary.operand, "operand");
            const CogIrValue *operand = cog_ir_get_value(function, instruction->as.unary.operand);
            if (!operand || !is_integer_type(module, operand->type) || operand->type != instruction->result_type) {
                ir_error(diagnostics, instruction->span, "%s requires matching integer operand/result", cog_ir_op_name(instruction->op));
                ok = 0;
            }
            break;
        }

        case COG_IR_OP_FADD:
        case COG_IR_OP_FSUB:
        case COG_IR_OP_FMUL:
        case COG_IR_OP_FDIV: {
            REQUIRE_VALUE(instruction->as.binary.lhs, "left");
            REQUIRE_VALUE(instruction->as.binary.rhs, "right");
            const CogIrValue *lhs = cog_ir_get_value(function, instruction->as.binary.lhs);
            const CogIrValue *rhs = cog_ir_get_value(function, instruction->as.binary.rhs);
            if (!lhs || !rhs || !is_float_type(module, lhs->type) || lhs->type != rhs->type || lhs->type != instruction->result_type) {
                ir_error(diagnostics, instruction->span, "%s requires matching float operands/result", cog_ir_op_name(instruction->op));
                ok = 0;
            }
            break;
        }

        case COG_IR_OP_FNEG: {
            REQUIRE_VALUE(instruction->as.unary.operand, "operand");
            const CogIrValue *operand = cog_ir_get_value(function, instruction->as.unary.operand);
            if (!operand || !is_float_type(module, operand->type) || operand->type != instruction->result_type) {
                ir_error(diagnostics, instruction->span, "fneg requires matching float operand/result");
                ok = 0;
            }
            break;
        }

        case COG_IR_OP_ICMP_EQ:
        case COG_IR_OP_ICMP_NE:
        case COG_IR_OP_ICMP_SLT:
        case COG_IR_OP_ICMP_SLE:
        case COG_IR_OP_ICMP_SGT:
        case COG_IR_OP_ICMP_SGE:
        case COG_IR_OP_ICMP_ULT:
        case COG_IR_OP_ICMP_ULE:
        case COG_IR_OP_ICMP_UGT:
        case COG_IR_OP_ICMP_UGE: {
            REQUIRE_VALUE(instruction->as.binary.lhs, "left");
            REQUIRE_VALUE(instruction->as.binary.rhs, "right");
            const CogIrValue *lhs = cog_ir_get_value(function, instruction->as.binary.lhs);
            const CogIrValue *rhs = cog_ir_get_value(function, instruction->as.binary.rhs);
            const CogIrType *lhs_type = lhs ? cog_ir_get_type(module, lhs->type) : NULL;
            const CogIrType *result_type = cog_ir_get_type(module, instruction->result_type);
            if (!lhs || !rhs || !lhs_type || lhs->type != rhs->type ||
                (lhs_type->kind != COG_IR_TYPE_INTEGER && lhs_type->kind != COG_IR_TYPE_ENUM && lhs_type->kind != COG_IR_TYPE_BOOL) ||
                !result_type || result_type->kind != COG_IR_TYPE_BOOL) {
                ir_error(diagnostics, instruction->span, "%s has invalid comparison operand/result types", cog_ir_op_name(instruction->op));
                ok = 0;
            }
            if (lhs_type && lhs_type->kind == COG_IR_TYPE_INTEGER) {
                int signed_pred = instruction->op >= COG_IR_OP_ICMP_SLT && instruction->op <= COG_IR_OP_ICMP_SGE;
                int unsigned_pred = instruction->op >= COG_IR_OP_ICMP_ULT && instruction->op <= COG_IR_OP_ICMP_UGE;
                if ((signed_pred && !lhs_type->as.integer.is_signed) || (unsigned_pred && lhs_type->as.integer.is_signed)) {
                    ir_error(diagnostics, instruction->span, "integer comparison signedness does not match operand type");
                    ok = 0;
                }
            }
            break;
        }

        case COG_IR_OP_FCMP_EQ:
        case COG_IR_OP_FCMP_NE:
        case COG_IR_OP_FCMP_LT:
        case COG_IR_OP_FCMP_LE:
        case COG_IR_OP_FCMP_GT:
        case COG_IR_OP_FCMP_GE: {
            REQUIRE_VALUE(instruction->as.binary.lhs, "left");
            REQUIRE_VALUE(instruction->as.binary.rhs, "right");
            const CogIrValue *lhs = cog_ir_get_value(function, instruction->as.binary.lhs);
            const CogIrValue *rhs = cog_ir_get_value(function, instruction->as.binary.rhs);
            const CogIrType *result_type = cog_ir_get_type(module, instruction->result_type);
            if (!lhs || !rhs || !is_float_type(module, lhs->type) || lhs->type != rhs->type ||
                !result_type || result_type->kind != COG_IR_TYPE_BOOL) {
                ir_error(diagnostics, instruction->span, "%s has invalid float comparison types", cog_ir_op_name(instruction->op));
                ok = 0;
            }
            break;
        }

        case COG_IR_OP_PTR_EQ:
        case COG_IR_OP_PTR_NE: {
            REQUIRE_VALUE(instruction->as.binary.lhs, "left");
            REQUIRE_VALUE(instruction->as.binary.rhs, "right");
            const CogIrValue *lhs = cog_ir_get_value(function, instruction->as.binary.lhs);
            const CogIrValue *rhs = cog_ir_get_value(function, instruction->as.binary.rhs);
            const CogIrType *result_type = cog_ir_get_type(module, instruction->result_type);
            if (!lhs || !rhs || lhs->type != rhs->type || !is_pointer_comparable_type(module, lhs->type) ||
                !result_type || result_type->kind != COG_IR_TYPE_BOOL) {
                ir_error(diagnostics, instruction->span, "%s has invalid pointer comparison types", cog_ir_op_name(instruction->op));
                ok = 0;
            }
            break;
        }

        case COG_IR_OP_BOOL_NOT: {
            REQUIRE_VALUE(instruction->as.unary.operand, "operand");
            const CogIrValue *operand = cog_ir_get_value(function, instruction->as.unary.operand);
            const CogIrType *type = operand ? cog_ir_get_type(module, operand->type) : NULL;
            if (!type || type->kind != COG_IR_TYPE_BOOL || operand->type != instruction->result_type) {
                ir_error(diagnostics, instruction->span, "bool.not requires bool operand/result");
                ok = 0;
            }
            break;
        }

        case COG_IR_OP_PTR_QUALIFY: {
            REQUIRE_VALUE(instruction->as.conversion.operand, "operand");
            const CogIrValue *operand = cog_ir_get_value(function, instruction->as.conversion.operand);
            const CogIrType *source = operand ? cog_ir_get_type(module, operand->type) : NULL;
            const CogIrType *target = cog_ir_get_type(module, instruction->as.conversion.target_type);
            int valid = operand && source && target &&
                        instruction->as.conversion.target_type == instruction->result_type;
            if (valid && source->kind == COG_IR_TYPE_POINTER && target->kind == COG_IR_TYPE_POINTER) {
                valid = source->as.pointer.pointee == target->as.pointer.pointee &&
                        (!source->as.pointer.is_readonly || target->as.pointer.is_readonly) &&
                        (!source->as.pointer.is_volatile || target->as.pointer.is_volatile);
            } else if (valid && source->kind == COG_IR_TYPE_OPAQUE_POINTER && target->kind == COG_IR_TYPE_OPAQUE_POINTER) {
                valid = (!source->as.opaque_pointer.is_readonly || target->as.opaque_pointer.is_readonly) &&
                        (!source->as.opaque_pointer.is_volatile || target->as.opaque_pointer.is_volatile);
            } else {
                valid = 0;
            }
            if (!valid) {
                ir_error(diagnostics, instruction->span, "ptr.qualify must preserve pointer identity and may only add qualifiers");
                ok = 0;
            }
            break;
        }

        case COG_IR_OP_CAST_CHECKED: {
            REQUIRE_VALUE(instruction->as.conversion.operand, "operand");
            const CogIrValue *operand = cog_ir_get_value(function, instruction->as.conversion.operand);
            const CogIrType *source = operand ? cog_ir_get_type(module, operand->type) : NULL;
            const CogIrType *target = cog_ir_get_type(module, instruction->result_type);
            int valid = operand && source && target &&
                        instruction->as.conversion.target_type == instruction->result_type;
            if (valid && operand->type != instruction->result_type) {
                int source_numeric = source->kind == COG_IR_TYPE_INTEGER || source->kind == COG_IR_TYPE_FLOAT;
                int target_numeric = target->kind == COG_IR_TYPE_INTEGER || target->kind == COG_IR_TYPE_FLOAT;
                valid = (source_numeric && target_numeric) ||
                        (source->kind == COG_IR_TYPE_ENUM && target->kind == COG_IR_TYPE_INTEGER);
            }
            if (!valid) {
                ir_error(diagnostics, instruction->span, "cast.checked has invalid source/target types");
                ok = 0;
            }
            break;
        }

        case COG_IR_OP_INT_TRUNCATE: {
            REQUIRE_VALUE(instruction->as.conversion.operand, "operand");
            const CogIrValue *operand = cog_ir_get_value(function, instruction->as.conversion.operand);
            const CogIrType *source = operand ? cog_ir_get_type(module, operand->type) : NULL;
            const CogIrType *target = cog_ir_get_type(module, instruction->result_type);
            if (!operand || !source || !target || source->kind != COG_IR_TYPE_INTEGER ||
                target->kind != COG_IR_TYPE_INTEGER ||
                instruction->as.conversion.target_type != instruction->result_type) {
                ir_error(diagnostics, instruction->span, "int.truncate requires integer source and target types");
                ok = 0;
            }
            break;
        }

        case COG_IR_OP_PTR_REINTERPRET: {
            REQUIRE_VALUE(instruction->as.conversion.operand, "operand");
            const CogIrValue *operand = cog_ir_get_value(function, instruction->as.conversion.operand);
            const CogIrType *source = operand ? cog_ir_get_type(module, operand->type) : NULL;
            const CogIrType *target = cog_ir_get_type(module, instruction->result_type);
            int source_raw = source && (source->kind == COG_IR_TYPE_POINTER || source->kind == COG_IR_TYPE_OPAQUE_POINTER);
            int target_raw = target && (target->kind == COG_IR_TYPE_POINTER || target->kind == COG_IR_TYPE_OPAQUE_POINTER);
            int source_ro = source && (source->kind == COG_IR_TYPE_POINTER ? source->as.pointer.is_readonly : source->as.opaque_pointer.is_readonly);
            int target_ro = target && (target->kind == COG_IR_TYPE_POINTER ? target->as.pointer.is_readonly : target->as.opaque_pointer.is_readonly);
            int source_vol = source && (source->kind == COG_IR_TYPE_POINTER ? source->as.pointer.is_volatile : source->as.opaque_pointer.is_volatile);
            int target_vol = target && (target->kind == COG_IR_TYPE_POINTER ? target->as.pointer.is_volatile : target->as.opaque_pointer.is_volatile);
            if (!operand || !source_raw || !target_raw || source->kind == target->kind ||
                (source_ro && !target_ro) || (source_vol && !target_vol) ||
                instruction->as.conversion.target_type != instruction->result_type) {
                ir_error(diagnostics, instruction->span, "ptr.reinterpret must cross typed/opaque raw pointers without discarding qualifiers");
                ok = 0;
            }
            break;
        }

        case COG_IR_OP_C_VARARG_PROMOTE: {
            REQUIRE_VALUE(instruction->as.conversion.operand, "operand");
            const CogIrValue *operand =
                cog_ir_get_value(function, instruction->as.conversion.operand);
            const CogIrType *source = operand
                ? cog_ir_get_type(module, operand->type)
                : NULL;
            const CogIrType *target = cog_ir_get_type(module, instruction->result_type);
            if (!operand ||
                instruction->as.conversion.target_type != instruction->result_type ||
                !c_vararg_promotion_valid(module, source, target)) {
                ir_error(
                    diagnostics,
                    instruction->span,
                    "c.vararg.promote does not match a required C default argument promotion"
                );
                ok = 0;
            }
            break;
        }

        case COG_IR_OP_CALL: {
            REQUIRE_VALUE(instruction->as.call.callee, "callee");
            const CogIrValue *callee = cog_ir_get_value(function, instruction->as.call.callee);
            const CogIrType *callee_type = callee ? cog_ir_get_type(module, callee->type) : NULL;
            if (!callee_type || callee_type->kind != COG_IR_TYPE_FUNCTION) {
                ir_error(diagnostics, instruction->span, "call callee is not a function value");
                ok = 0;
                break;
            }
            if (callee_type->as.function.abi == COG_IR_ABI_C) {
                const CogIrAbiType *callee_abi = callee && callee->abi_type != COG_IR_ABI_TYPE_INVALID
                    ? cog_ir_get_abi_type(module, callee->abi_type)
                    : NULL;
                if (!callee_abi || callee_abi->kind != COG_IR_ABI_TYPE_FUNCTION ||
                    callee_abi->runtime_type != callee->type) {
                    ir_error(diagnostics, instruction->span,
                             "C call callee is missing exact function-pointer ABI metadata");
                    ok = 0;
                }
            }
            if (instruction->as.call.argument_count < callee_type->as.function.parameter_count ||
                (!callee_type->as.function.is_variadic && instruction->as.call.argument_count != callee_type->as.function.parameter_count)) {
                ir_error(diagnostics, instruction->span, "call argument count does not match function signature");
                ok = 0;
            }
            for (size_t i = 0; i < instruction->as.call.argument_count; ++i) {
                REQUIRE_VALUE(instruction->as.call.arguments[i], "call argument");
                const CogIrValue *argument = cog_ir_get_value(function, instruction->as.call.arguments[i]);
                if (i < callee_type->as.function.parameter_count) {
                    if (!argument || argument->type != callee_type->as.function.parameter_types[i]) {
                        ir_error(diagnostics, instruction->span, "call argument %zu has wrong type", i);
                        ok = 0;
                    }
                } else if (callee_type->as.function.is_variadic &&
                           callee_type->as.function.abi == COG_IR_ABI_C &&
                           (!argument || !c_vararg_argument_type_is_legal(module, argument->type))) {
                    ir_error(
                        diagnostics,
                        instruction->span,
                        "C variadic call argument %zu is not default-promoted or ABI-legal",
                        i
                    );
                    ok = 0;
                }
            }
            const CogIrType *return_type = cog_ir_get_type(module, callee_type->as.function.result_type);
            if (!return_type) {
                ok = 0;
            } else if (return_type->kind == COG_IR_TYPE_VOID) {
                if (instruction->result_type != COG_IR_TYPE_INVALID) {
                    ir_error(diagnostics, instruction->span, "void call unexpectedly has a result");
                    ok = 0;
                }
            } else if (instruction->result_type != callee_type->as.function.result_type) {
                ir_error(diagnostics, instruction->span, "call result type does not match function result");
                ok = 0;
            }
            break;
        }

        case COG_IR_OP_FIELD_ADDR: {
            REQUIRE_VALUE(instruction->as.field_addr.base, "base");
            const CogIrValue *base = cog_ir_get_value(function, instruction->as.field_addr.base);
            CogIrTypeId aggregate_id = COG_IR_TYPE_INVALID;
            int base_readonly = 0, base_volatile = 0;
            const CogIrType *aggregate = NULL;
            CogIrTypeId result_pointee = COG_IR_TYPE_INVALID;
            int result_readonly = 0, result_volatile = 0;
            if (base && pointer_pointee(module, base->type, &aggregate_id, &base_readonly, &base_volatile))
                aggregate = cog_ir_get_type(module, aggregate_id);
            int valid = aggregate &&
                        (aggregate->kind == COG_IR_TYPE_STRUCT || aggregate->kind == COG_IR_TYPE_UNION) &&
                        instruction->as.field_addr.field_index < aggregate->as.aggregate.field_count &&
                        pointer_pointee(module, instruction->result_type, &result_pointee, &result_readonly, &result_volatile) &&
                        result_pointee == aggregate->as.aggregate.fields[instruction->as.field_addr.field_index].type &&
                        result_readonly == base_readonly && result_volatile == base_volatile;
            if (!valid) {
                ir_error(diagnostics, instruction->span, "field_addr has invalid aggregate field or access qualifiers");
                ok = 0;
            }
            break;
        }

        case COG_IR_OP_ARRAY_ELEM_ADDR: {
            REQUIRE_VALUE(instruction->as.index_addr.base, "base");
            REQUIRE_VALUE(instruction->as.index_addr.index, "index");
            const CogIrValue *base = cog_ir_get_value(function, instruction->as.index_addr.base);
            const CogIrValue *index = cog_ir_get_value(function, instruction->as.index_addr.index);
            CogIrTypeId array_id = COG_IR_TYPE_INVALID, result_pointee = COG_IR_TYPE_INVALID;
            int base_ro = 0, base_vol = 0, result_ro = 0, result_vol = 0;
            const CogIrType *array = NULL;
            if (base && pointer_pointee(module, base->type, &array_id, &base_ro, &base_vol))
                array = cog_ir_get_type(module, array_id);
            int valid = array && array->kind == COG_IR_TYPE_ARRAY && index && is_integer_type(module, index->type) &&
                        pointer_pointee(module, instruction->result_type, &result_pointee, &result_ro, &result_vol) &&
                        result_pointee == array->as.array.element_type && result_ro == base_ro && result_vol == base_vol;
            if (!valid) {
                ir_error(diagnostics, instruction->span, "array_elem_addr has invalid array base/index/result type");
                ok = 0;
            }
            break;
        }

        case COG_IR_OP_PTR_INDEX_ADDR: {
            REQUIRE_VALUE(instruction->as.index_addr.base, "base");
            REQUIRE_VALUE(instruction->as.index_addr.index, "index");
            const CogIrValue *base = cog_ir_get_value(function, instruction->as.index_addr.base);
            const CogIrValue *index = cog_ir_get_value(function, instruction->as.index_addr.index);
            if (!base || !index || !is_integer_type(module, index->type) || base->type != instruction->result_type ||
                !pointer_pointee(module, base->type, NULL, NULL, NULL)) {
                ir_error(diagnostics, instruction->span, "ptr_index_addr requires typed pointer base, integer index, and matching result pointer");
                ok = 0;
            }
            break;
        }

        case COG_IR_OP_MAKE_STRUCT: {
            const CogIrType *type = cog_ir_get_type(module, instruction->result_type);
            if (!type || type->kind != COG_IR_TYPE_STRUCT || !type->as.aggregate.is_complete ||
                instruction->as.aggregate.value_count != type->as.aggregate.field_count) {
                ir_error(diagnostics, instruction->span, "make_struct has invalid result type or field count");
                ok = 0;
                break;
            }
            for (size_t i = 0; i < instruction->as.aggregate.value_count; ++i) {
                REQUIRE_VALUE(instruction->as.aggregate.values[i], "field");
                const CogIrValue *value = cog_ir_get_value(function, instruction->as.aggregate.values[i]);
                if (!value || value->type != type->as.aggregate.fields[i].type) {
                    ir_error(diagnostics, instruction->span, "make_struct field %zu has wrong type", i);
                    ok = 0;
                }
            }
            break;
        }

        case COG_IR_OP_MAKE_ARRAY: {
            const CogIrType *type = cog_ir_get_type(module, instruction->result_type);
            if (!type || type->kind != COG_IR_TYPE_ARRAY || instruction->as.aggregate.value_count != type->as.array.length) {
                ir_error(diagnostics, instruction->span, "make_array has invalid result type or element count");
                ok = 0;
                break;
            }
            for (size_t i = 0; i < instruction->as.aggregate.value_count; ++i) {
                REQUIRE_VALUE(instruction->as.aggregate.values[i], "element");
                const CogIrValue *value = cog_ir_get_value(function, instruction->as.aggregate.values[i]);
                if (!value || value->type != type->as.array.element_type) {
                    ir_error(diagnostics, instruction->span, "make_array element %zu has wrong type", i);
                    ok = 0;
                }
            }
            break;
        }

        case COG_IR_OP_EXTRACT_FIELD: {
            REQUIRE_VALUE(instruction->as.extract.aggregate, "aggregate");
            const CogIrValue *value = cog_ir_get_value(function, instruction->as.extract.aggregate);
            const CogIrType *type = value ? cog_ir_get_type(module, value->type) : NULL;
            if (!type || (type->kind != COG_IR_TYPE_STRUCT && type->kind != COG_IR_TYPE_UNION) ||
                instruction->as.extract.index >= type->as.aggregate.field_count ||
                instruction->result_type != type->as.aggregate.fields[instruction->as.extract.index].type) {
                ir_error(diagnostics, instruction->span, "extract_field has invalid aggregate/index/result type");
                ok = 0;
            }
            break;
        }

        case COG_IR_OP_EXTRACT_ELEMENT: {
            REQUIRE_VALUE(instruction->as.extract.aggregate, "aggregate");
            const CogIrValue *value = cog_ir_get_value(function, instruction->as.extract.aggregate);
            const CogIrType *type = value ? cog_ir_get_type(module, value->type) : NULL;
            if (!type || type->kind != COG_IR_TYPE_ARRAY || instruction->as.extract.index >= type->as.array.length ||
                instruction->result_type != type->as.array.element_type) {
                ir_error(diagnostics, instruction->span, "extract_element has invalid array/index/result type");
                ok = 0;
            }
            break;
        }
    }

#undef REQUIRE_VALUE
    return ok;
}

static int verify_function(
    const CogIrModule *module,
    const CogIrFunction *function,
    DiagnosticList *diagnostics
) {
    int ok = 1;
    const CogIrType *function_type = cog_ir_get_type(module, function->type);
    if (!function_type || function_type->kind != COG_IR_TYPE_FUNCTION) {
        ir_error(diagnostics, function->span, "function @f%u has invalid function type", function->id);
        return 0;
    }

    if (!valid_span(module, function->span)) {
        ir_error(diagnostics, source_span_invalid(), "function @f%u has invalid source span", function->id);
        ok = 0;
    }

    if (function->abi.abi != function_type->as.function.abi ||
        function->abi.calling_convention != function_type->as.function.calling_convention ||
        !!function->abi.is_variadic != !!function_type->as.function.is_variadic ||
        function->parameter_count != function_type->as.function.parameter_count) {
        ir_error(diagnostics, function->span, "function @f%u ABI/signature metadata disagrees with its function type", function->id);
        ok = 0;
    }


    if (function->linkage == COG_IR_LINKAGE_EXTERNAL && function->kind != COG_IR_FUNCTION_DECLARATION) {
        ir_error(diagnostics, function->span, "external function @f%u must be a declaration", function->id);
        ok = 0;
    }

    for (size_t i = 0; i < function->value_count; ++i) {
        const CogIrValue *value = &function->values[i];
        if (!optional_abi_matches_runtime(module, value->abi_type, value->type)) {
            ir_error(diagnostics, value->span,
                     "value %%%zu in function @f%u has ABI metadata for the wrong runtime type",
                     i, function->id);
            ok = 0;
        }
    }

    if (function->kind == COG_IR_FUNCTION_DECLARATION) {
        if (function->block_count || function->slot_count || function->entry_block != COG_IR_BLOCK_INVALID) {
            ir_error(diagnostics, function->span, "function declaration @f%u unexpectedly has a body", function->id);
            ok = 0;
        }
        return ok;
    }

    if (!function->block_count || function->entry_block == COG_IR_BLOCK_INVALID ||
        !cog_ir_get_block(function, function->entry_block)) {
        ir_error(diagnostics, function->span, "function definition @f%u has no valid entry block", function->id);
        ok = 0;
    }

    for (size_t i = 0; i < function->parameter_count; ++i) {
        const CogIrValue *value = cog_ir_get_value(function, function->parameters[i]);
        if (!value || value->kind != COG_IR_VALUE_FUNCTION_PARAMETER || value->ordinal != i ||
            value->type != function_type->as.function.parameter_types[i]) {
            ir_error(diagnostics, function->span, "function @f%u parameter %zu has invalid value metadata", function->id, i);
            ok = 0;
        }
    }

    for (size_t i = 0; i < function->slot_count; ++i) {
        const CogIrSlot *slot = &function->slots[i];
        int valid_kind = slot->kind >= COG_IR_SLOT_SOURCE_LOCAL &&
                         slot->kind <= COG_IR_SLOT_COMPILER_TEMP;
        int valid_parameter = slot->kind == COG_IR_SLOT_SOURCE_PARAMETER
            ? slot->parameter_index < function->parameter_count &&
              slot->type == function_type->as.function.parameter_types[slot->parameter_index]
            : slot->parameter_index == COG_IR_PARAMETER_INDEX_INVALID;
        if (slot->id != i || !valid_kind || !valid_parameter ||
            !cog_ir_get_type(module, slot->type) || !valid_span(module, slot->span) ||
            !optional_abi_matches_runtime(module, slot->abi_type, slot->type)) {
            ir_error(diagnostics, slot->span, "slot $s%zu in function @f%u is invalid", i, function->id);
            ok = 0;
        }
    }

    for (size_t b = 0; b < function->block_count; ++b) {
        const CogIrBlock *block = &function->blocks[b];
        if (block->id != b || !valid_span(module, block->span)) {
            ir_error(diagnostics, block->span, "block %zu in function @f%u has invalid identity/span", b, function->id);
            ok = 0;
        }

        for (size_t p = 0; p < block->parameter_count; ++p) {
            const CogIrBlockParam *param = &block->parameters[p];
            const CogIrValue *value = cog_ir_get_value(function, param->value);
            if (!value || value->kind != COG_IR_VALUE_BLOCK_PARAMETER || value->block != block->id ||
                value->ordinal != p || value->type != param->type || !cog_ir_get_type(module, param->type)) {
                ir_error(diagnostics, param->span, "block %u parameter %zu has invalid value metadata", block->id, p);
                ok = 0;
            }
        }

        for (size_t i = 0; i < block->instruction_count; ++i)
            if (!verify_instruction(module, function, block, i, &block->instructions[i], diagnostics))
                ok = 0;

        const CogIrTerminator *term = &block->terminator;
        if (term->kind == COG_IR_TERMINATOR_NONE) {
            ir_error(diagnostics, block->span, "block %u in function @f%u has no terminator", block->id, function->id);
            ok = 0;
            continue;
        }
        if (!valid_span(module, term->span)) {
            ir_error(diagnostics, source_span_invalid(), "terminator in block %u has invalid source span", block->id);
            ok = 0;
        }

        switch (term->kind) {
            case COG_IR_TERMINATOR_BR:
                if (!verify_edge(module, function, block->id, &term->as.branch.edge, term->span, diagnostics)) ok = 0;
                break;

            case COG_IR_TERMINATOR_COND_BR: {
                const CogIrValue *condition = cog_ir_get_value(function, term->as.cond_branch.condition);
                const CogIrType *condition_type = condition ? cog_ir_get_type(module, condition->type) : NULL;
                if (!condition || !terminator_value_available(function, block->id, condition->id) ||
                    !condition_type || condition_type->kind != COG_IR_TYPE_BOOL) {
                    ir_error(diagnostics, term->span, "cond_br condition is not an available bool value");
                    ok = 0;
                }
                if (!verify_edge(module, function, block->id, &term->as.cond_branch.if_true, term->span, diagnostics)) ok = 0;
                if (!verify_edge(module, function, block->id, &term->as.cond_branch.if_false, term->span, diagnostics)) ok = 0;
                break;
            }

            case COG_IR_TERMINATOR_SWITCH: {
                const CogIrValue *value = cog_ir_get_value(function, term->as.switch_term.value);
                const CogIrType *value_type = value ? cog_ir_get_type(module, value->type) : NULL;
                if (!value || !terminator_value_available(function, block->id, value->id) ||
                    !value_type || (value_type->kind != COG_IR_TYPE_INTEGER &&
                                    value_type->kind != COG_IR_TYPE_BOOL &&
                                    value_type->kind != COG_IR_TYPE_ENUM)) {
                    ir_error(diagnostics, term->span, "switch value is not an available integer, bool, or enum value");
                    ok = 0;
                }
                for (size_t i = 0; i < term->as.switch_term.case_count; ++i) {
                    const CogIrConstant *key = cog_ir_get_constant(module, term->as.switch_term.cases[i].key);
                    if (!key || !value || key->type != value->type) {
                        ir_error(diagnostics, term->span, "switch case %zu has wrong key type", i);
                        ok = 0;
                    }
                    for (size_t j = 0; j < i; ++j)
                        if (term->as.switch_term.cases[j].key == term->as.switch_term.cases[i].key) {
                            ir_error(diagnostics, term->span, "switch has duplicate case constant @c%u", term->as.switch_term.cases[i].key);
                            ok = 0;
                        }
                    if (!verify_edge(module, function, block->id, &term->as.switch_term.cases[i].edge, term->span, diagnostics)) ok = 0;
                }
                if (!verify_edge(module, function, block->id, &term->as.switch_term.default_edge, term->span, diagnostics)) ok = 0;
                break;
            }

            case COG_IR_TERMINATOR_RET: {
                const CogIrType *return_type = cog_ir_get_type(module, function_type->as.function.result_type);
                if (!return_type) {
                    ok = 0;
                } else if (return_type->kind == COG_IR_TYPE_VOID) {
                    if (term->as.ret.has_value) {
                        ir_error(diagnostics, term->span, "void function @f%u returns a value", function->id);
                        ok = 0;
                    }
                } else {
                    const CogIrValue *value = term->as.ret.has_value
                        ? cog_ir_get_value(function, term->as.ret.value) : NULL;
                    if (!value || !terminator_value_available(function, block->id, term->as.ret.value) ||
                        value->type != function_type->as.function.result_type) {
                        ir_error(diagnostics, term->span, "function @f%u return value has wrong type or is unavailable", function->id);
                        ok = 0;
                    }
                }
                break;
            }

            case COG_IR_TERMINATOR_TRAP:
            case COG_IR_TERMINATOR_UNREACHABLE:
            case COG_IR_TERMINATOR_NONE:
                break;
        }
    }

    return ok;
}

int cog_ir_verify(const CogIrModule *module, DiagnosticList *diagnostics)
{
    if (!module || !module->arena)
        return 0;

    int ok = 1;
    char target_message[256];
    if (!target_info_validate(&module->target, target_message, sizeof(target_message))) {
        ir_error(diagnostics, source_span_invalid(), "invalid CogIR target: %s", target_message);
        ok = 0;
    }

    for (size_t i = 0; i < module->type_count; ++i) {
        const CogIrType *type = &module->types[i];
        if (type->id != i || !valid_span(module, type->span)) {
            ir_error(diagnostics, type->span, "type %%t%zu has invalid identity/span", i);
            ok = 0;
        }

        switch (type->kind) {
            case COG_IR_TYPE_VOID:
            case COG_IR_TYPE_BOOL:
                break;
            case COG_IR_TYPE_INTEGER:
                if (type->as.integer.bits != 8 && type->as.integer.bits != 16 &&
                    type->as.integer.bits != 32 && type->as.integer.bits != 64) {
                    ir_error(diagnostics, type->span, "integer type %%t%zu has unsupported width", i);
                    ok = 0;
                }
                break;
            case COG_IR_TYPE_FLOAT:
                if (type->as.floating.bits != 32 && type->as.floating.bits != 64) {
                    ir_error(diagnostics, type->span, "float type %%t%zu has unsupported width", i);
                    ok = 0;
                }
                break;
            case COG_IR_TYPE_POINTER:
                if (!cog_ir_get_type(module, type->as.pointer.pointee)) {
                    ir_error(diagnostics, type->span, "pointer type %%t%zu has invalid pointee", i);
                    ok = 0;
                }
                break;
            case COG_IR_TYPE_OPAQUE_POINTER:
                break;
            case COG_IR_TYPE_ARRAY:
                if (!cog_ir_get_type(module, type->as.array.element_type)) {
                    ir_error(diagnostics, type->span, "array type %%t%zu has invalid element type", i);
                    ok = 0;
                }
                break;
            case COG_IR_TYPE_STRUCT:
            case COG_IR_TYPE_UNION:
                if (type->as.aggregate.is_complete == type->as.aggregate.is_incomplete) {
                    ir_error(diagnostics, type->span, "nominal aggregate %%t%zu is neither exactly defined nor intentionally incomplete", i);
                    ok = 0;
                }
                if (type->as.aggregate.is_incomplete && !type->as.aggregate.is_repr_c) {
                    ir_error(diagnostics, type->span, "incomplete aggregate %%t%zu must be repr(c)", i);
                    ok = 0;
                }
                for (size_t f = 0; f < type->as.aggregate.field_count; ++f) {
                    if (!cog_ir_get_type(module, type->as.aggregate.fields[f].type) ||
                        !valid_span(module, type->as.aggregate.fields[f].span)) {
                        ir_error(diagnostics, type->as.aggregate.fields[f].span, "field %zu of aggregate %%t%zu is invalid", f, i);
                        ok = 0;
                    }
                }
                break;
            case COG_IR_TYPE_ENUM: {
                const CogIrType *backing = cog_ir_get_type(module, type->as.enumeration.backing_type);
                if (!backing || backing->kind != COG_IR_TYPE_INTEGER) {
                    ir_error(diagnostics, type->span, "enum type %%t%zu has invalid backing type", i);
                    ok = 0;
                }
                if (type->as.enumeration.is_repr_c && !cog_ir_get_abi_type(module, type->as.enumeration.backing_abi_type)) {
                    ir_error(diagnostics, type->span, "repr(c) enum %%t%zu lacks valid backing ABI type", i);
                    ok = 0;
                }
                break;
            }
            case COG_IR_TYPE_FUNCTION:
                if (type->as.function.is_variadic &&
                    (type->as.function.abi != COG_IR_ABI_C ||
                     type->as.function.calling_convention == COG_IR_CALL_STDCALL)) {
                    ir_error(
                        diagnostics,
                        type->span,
                        "variadic function type %%t%zu must use the C ABI and may not use stdcall",
                        i
                    );
                    ok = 0;
                }
                if (!cog_ir_get_type(module, type->as.function.result_type)) {
                    ir_error(diagnostics, type->span, "function type %%t%zu has invalid return type", i);
                    ok = 0;
                }
                for (size_t p = 0; p < type->as.function.parameter_count; ++p) {
                    const CogIrType *parameter = cog_ir_get_type(module, type->as.function.parameter_types[p]);
                    if (!parameter || parameter->kind == COG_IR_TYPE_VOID) {
                        ir_error(diagnostics, type->span, "function type %%t%zu parameter %zu is invalid", i, p);
                        ok = 0;
                    }
                }
                break;
        }
    }

    for (size_t i = 0; i < module->abi_type_count; ++i) {
        const CogIrAbiType *type = &module->abi_types[i];
        if (type->id != i || !cog_ir_get_type(module, type->runtime_type)) {
            ir_error(diagnostics, source_span_invalid(), "ABI type %%a%zu has invalid identity/runtime type", i);
            ok = 0;
        }
    }

    for (size_t i = 0; i < module->constant_count; ++i) {
        if (module->constants[i].id != i || !constant_matches_type(module, &module->constants[i], diagnostics))
            ok = 0;
    }

    for (size_t i = 0; i < module->global_count; ++i) {
        const CogIrGlobal *global = &module->globals[i];
        const CogIrConstant *initializer = cog_ir_get_constant(module, global->static_initializer);
        if (global->id != i || !cog_ir_get_type(module, global->type) || !initializer ||
            initializer->type != global->type || !valid_span(module, global->span) ||
            !optional_abi_matches_runtime(module, global->abi_type, global->type)) {
            ir_error(diagnostics, global->span, "global @g%zu has invalid type/initializer/span", i);
            ok = 0;
        }
        if (!global->is_compiler_generated && initializer && initializer->kind != COG_IR_CONST_ZERO) {
            ir_error(diagnostics, global->span, "source global @g%zu must use zero static initialization in CogIR v1", i);
            ok = 0;
        }
    }

    for (size_t i = 0; i < module->function_count; ++i) {
        if (module->functions[i].id != i || !verify_function(module, &module->functions[i], diagnostics))
            ok = 0;
    }

    if (module->entry_function != COG_IR_FUNCTION_INVALID) {
        const CogIrFunction *entry = cog_ir_get_function(module, module->entry_function);
        const CogIrType *type = entry ? cog_ir_get_type(module, entry->type) : NULL;
        const CogIrType *result = type && type->kind == COG_IR_TYPE_FUNCTION
            ? cog_ir_get_type(module, type->as.function.result_type) : NULL;
        if (!entry || !type || type->kind != COG_IR_TYPE_FUNCTION ||
            entry->kind != COG_IR_FUNCTION_DEFINITION ||
            entry->linkage != COG_IR_LINKAGE_INTERNAL ||
            entry->is_compiler_generated || module->entry_function == module->init_function ||
            type->as.function.abi != COG_IR_ABI_COGLET ||
            type->as.function.calling_convention != COG_IR_CALL_DEFAULT ||
            type->as.function.is_variadic || type->as.function.parameter_count != 0 ||
            !result || result->kind != COG_IR_TYPE_INTEGER ||
            result->as.integer.bits != 32 || !result->as.integer.is_signed) {
            ir_error(
                diagnostics,
                entry ? entry->span : source_span_invalid(),
                "module entry function must be an internal Coglet () -> s32 definition distinct from module init"
            );
            ok = 0;
        }
    }

    if (module->init_function != COG_IR_FUNCTION_INVALID) {
        const CogIrFunction *init = cog_ir_get_function(module, module->init_function);
        const CogIrType *type = init ? cog_ir_get_type(module, init->type) : NULL;
        const CogIrType *result = type && type->kind == COG_IR_TYPE_FUNCTION
            ? cog_ir_get_type(module, type->as.function.result_type) : NULL;
        if (!init || !type || type->kind != COG_IR_TYPE_FUNCTION ||
            init->kind != COG_IR_FUNCTION_DEFINITION || init->linkage != COG_IR_LINKAGE_INTERNAL ||
            !init->is_compiler_generated || type->as.function.abi != COG_IR_ABI_COGLET ||
            type->as.function.calling_convention != COG_IR_CALL_DEFAULT || type->as.function.is_variadic ||
            type->as.function.parameter_count != 0 || !result || result->kind != COG_IR_TYPE_VOID) {
            ir_error(diagnostics, init ? init->span : source_span_invalid(), "module init function must be compiler-generated internal Coglet () -> void definition");
            ok = 0;
        }
    }

    return ok;
}
