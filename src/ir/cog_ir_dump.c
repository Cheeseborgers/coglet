#include "ir/cog_ir.h"

#include <inttypes.h>

static void dump_sv(FILE *stream, StringView view)
{
    if (view.data && view.length)
        fprintf(stream, "%.*s", (int)view.length, view.data);
}

static void dump_type_ref(FILE *stream, const CogIrModule *module, CogIrTypeId id)
{
    const CogIrType *type = cog_ir_get_type(module, id);
    if (!type) {
        fprintf(stream, "<bad-type:%u>", id);
        return;
    }

    switch (type->kind) {
        case COG_IR_TYPE_VOID: fprintf(stream, "void"); break;
        case COG_IR_TYPE_BOOL: fprintf(stream, "bool"); break;
        case COG_IR_TYPE_INTEGER:
            fprintf(stream, "%c%u", type->as.integer.is_signed ? 's' : 'u', type->as.integer.bits);
            break;
        case COG_IR_TYPE_FLOAT:
            fprintf(stream, "f%u", type->as.floating.bits);
            break;
        case COG_IR_TYPE_POINTER:
            if (type->as.pointer.is_readonly) fprintf(stream, "readonly ");
            if (type->as.pointer.is_volatile) fprintf(stream, "volatile ");
            fprintf(stream, "ptr<");
            dump_type_ref(stream, module, type->as.pointer.pointee);
            fprintf(stream, ">");
            break;
        case COG_IR_TYPE_OPAQUE_POINTER:
            if (type->as.opaque_pointer.is_readonly) fprintf(stream, "readonly ");
            if (type->as.opaque_pointer.is_volatile) fprintf(stream, "volatile ");
            fprintf(stream, "opaque_ptr");
            break;
        case COG_IR_TYPE_ARRAY:
            fprintf(stream, "[%zu]", type->as.array.length);
            dump_type_ref(stream, module, type->as.array.element_type);
            break;
        case COG_IR_TYPE_STRUCT:
        case COG_IR_TYPE_UNION:
        case COG_IR_TYPE_ENUM:
            fprintf(stream, "%%t%u", id);
            break;
        case COG_IR_TYPE_FUNCTION:
            fprintf(stream, "fn(");
            for (size_t i = 0; i < type->as.function.parameter_count; ++i) {
                if (i) fprintf(stream, ", ");
                dump_type_ref(stream, module, type->as.function.parameter_types[i]);
            }
            if (type->as.function.is_variadic) {
                if (type->as.function.parameter_count) fprintf(stream, ", ");
                fprintf(stream, "...");
            }
            fprintf(stream, ") -> ");
            dump_type_ref(stream, module, type->as.function.result_type);
            break;
    }
}

static const char *abi_name(CogIrAbiRepresentation abi)
{
    return abi == COG_IR_ABI_C ? "c" : "coglet";
}

static const char *linkage_name(CogIrLinkage linkage)
{
    return linkage == COG_IR_LINKAGE_EXTERNAL ? "external" : "internal";
}

static void dump_constant(FILE *stream, const CogIrModule *module, CogIrConstId id)
{
    const CogIrConstant *constant = cog_ir_get_constant(module, id);
    if (!constant) {
        fprintf(stream, "<bad-const:%u>", id);
        return;
    }

    switch (constant->kind) {
        case COG_IR_CONST_ZERO: fprintf(stream, "zeroinit"); break;
        case COG_IR_CONST_BOOL: fprintf(stream, "%s", constant->as.boolean ? "true" : "false"); break;
        case COG_IR_CONST_INTEGER: fprintf(stream, "0x%" PRIx64, constant->as.integer_bits); break;
        case COG_IR_CONST_FLOAT32: fprintf(stream, "f32bits(0x%08" PRIx32 ")", constant->as.float32_bits); break;
        case COG_IR_CONST_FLOAT64: fprintf(stream, "f64bits(0x%016" PRIx64 ")", constant->as.float64_bits); break;
        case COG_IR_CONST_NULL: fprintf(stream, "null"); break;
        case COG_IR_CONST_ARRAY:
        case COG_IR_CONST_STRUCT:
            fprintf(stream, constant->kind == COG_IR_CONST_ARRAY ? "[" : "{");
            for (size_t i = 0; i < constant->as.aggregate.element_count; ++i) {
                if (i) fprintf(stream, ", ");
                fprintf(stream, "@c%u", constant->as.aggregate.elements[i]);
            }
            fprintf(stream, constant->kind == COG_IR_CONST_ARRAY ? "]" : "}");
            break;
    }
}

static void dump_type_declarations(FILE *stream, const CogIrModule *module)
{
    for (size_t i = 0; i < module->type_count; ++i) {
        const CogIrType *type = &module->types[i];
        if (type->kind != COG_IR_TYPE_STRUCT && type->kind != COG_IR_TYPE_UNION && type->kind != COG_IR_TYPE_ENUM)
            continue;

        fprintf(stream, "type %%t%zu", i);
        if (!string_view_is_empty(type->debug_name)) {
            fprintf(stream, " \"");
            dump_sv(stream, type->debug_name);
            fprintf(stream, "\"");
        }
        fprintf(stream, " = ");

        if (type->kind == COG_IR_TYPE_ENUM) {
            fprintf(stream, "enum(");
            dump_type_ref(stream, module, type->as.enumeration.backing_type);
            fprintf(stream, ") {");
            for (size_t m = 0; m < type->as.enumeration.member_count; ++m) {
                if (m) fprintf(stream, ", ");
                dump_sv(stream, type->as.enumeration.members[m].debug_name);
                fprintf(stream, "=0x%" PRIx64, type->as.enumeration.members[m].bits);
            }
            fprintf(stream, "}");
        } else if (type->as.aggregate.is_incomplete) {
            fprintf(stream, "%s incomplete", type->kind == COG_IR_TYPE_STRUCT ? "struct" : "union");
        } else {
            fprintf(stream, "%s {", type->kind == COG_IR_TYPE_STRUCT ? "struct" : "union");
            for (size_t f = 0; f < type->as.aggregate.field_count; ++f) {
                if (f) fprintf(stream, ", ");
                dump_sv(stream, type->as.aggregate.fields[f].debug_name);
                fprintf(stream, ": ");
                dump_type_ref(stream, module, type->as.aggregate.fields[f].type);
            }
            fprintf(stream, "}");
        }
        fprintf(stream, "\n");
    }
}

static void dump_instruction(FILE *stream, const CogIrModule *module, const CogIrInstruction *instruction)
{
    if (instruction->result != COG_IR_VALUE_INVALID)
        fprintf(stream, "    %%%u = ", instruction->result);
    else
        fprintf(stream, "    ");

    fprintf(stream, "%s", cog_ir_op_name(instruction->op));
    switch (instruction->op) {
        case COG_IR_OP_CONST:
            fprintf(stream, " ");
            dump_type_ref(stream, module, instruction->result_type);
            fprintf(stream, " @c%u(", instruction->as.constant.constant);
            dump_constant(stream, module, instruction->as.constant.constant);
            fprintf(stream, ")");
            break;
        case COG_IR_OP_FUNCTION_REF: fprintf(stream, " @f%u", instruction->as.function_ref.function); break;
        case COG_IR_OP_SIZE_OF:
        case COG_IR_OP_ALIGN_OF:
            fprintf(stream, " ");
            dump_type_ref(stream, module, instruction->as.type_query.queried_type);
            break;
        case COG_IR_OP_LOCAL_ADDR: fprintf(stream, " $s%u", instruction->as.local_addr.slot); break;
        case COG_IR_OP_GLOBAL_ADDR: fprintf(stream, " @g%u", instruction->as.global_addr.global); break;
        case COG_IR_OP_LOAD:
            fprintf(stream, "%s %%%u", instruction->as.load.is_volatile ? ".volatile" : "", instruction->as.load.address);
            break;
        case COG_IR_OP_STORE:
            fprintf(stream, "%s %%%u, %%%u", instruction->as.store.is_volatile ? ".volatile" : "",
                    instruction->as.store.address, instruction->as.store.value);
            break;
        case COG_IR_OP_INEG_CHECKED:
        case COG_IR_OP_INEG_WRAP:
        case COG_IR_OP_BIT_NOT:
        case COG_IR_OP_FNEG:
        case COG_IR_OP_BOOL_NOT:
            fprintf(stream, " %%%u", instruction->as.unary.operand);
            break;
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
        case COG_IR_OP_FADD:
        case COG_IR_OP_FSUB:
        case COG_IR_OP_FMUL:
        case COG_IR_OP_FDIV:
        case COG_IR_OP_ICMP_EQ:
        case COG_IR_OP_ICMP_NE:
        case COG_IR_OP_ICMP_SLT:
        case COG_IR_OP_ICMP_SLE:
        case COG_IR_OP_ICMP_SGT:
        case COG_IR_OP_ICMP_SGE:
        case COG_IR_OP_ICMP_ULT:
        case COG_IR_OP_ICMP_ULE:
        case COG_IR_OP_ICMP_UGT:
        case COG_IR_OP_ICMP_UGE:
        case COG_IR_OP_FCMP_EQ:
        case COG_IR_OP_FCMP_NE:
        case COG_IR_OP_FCMP_LT:
        case COG_IR_OP_FCMP_LE:
        case COG_IR_OP_FCMP_GT:
        case COG_IR_OP_FCMP_GE:
        case COG_IR_OP_PTR_EQ:
        case COG_IR_OP_PTR_NE:
            fprintf(stream, " %%%u, %%%u", instruction->as.binary.lhs, instruction->as.binary.rhs);
            break;
        case COG_IR_OP_CAST_CHECKED:
        case COG_IR_OP_INT_TRUNCATE:
        case COG_IR_OP_PTR_REINTERPRET:
        case COG_IR_OP_PTR_QUALIFY:
        case COG_IR_OP_C_VARARG_PROMOTE:
            fprintf(stream, " %%%u to ", instruction->as.conversion.operand);
            dump_type_ref(stream, module, instruction->as.conversion.target_type);
            break;
        case COG_IR_OP_ASM:
            fprintf(stream, " %s(\"", instruction->as.asm_stmt.is_volatile ? "volatile" : "");
            dump_sv(stream, instruction->as.asm_stmt.text);
            fprintf(stream, "\")");
            break;
        case COG_IR_OP_CALL:
            fprintf(stream, " %%%u(", instruction->as.call.callee);
            for (size_t i = 0; i < instruction->as.call.argument_count; ++i) {
                if (i) fprintf(stream, ", ");
                fprintf(stream, "%%%u", instruction->as.call.arguments[i]);
            }
            fprintf(stream, ")");
            break;
        case COG_IR_OP_FIELD_ADDR:
            fprintf(stream, " %%%u, field %u", instruction->as.field_addr.base, instruction->as.field_addr.field_index);
            break;
        case COG_IR_OP_ARRAY_ELEM_ADDR:
        case COG_IR_OP_PTR_INDEX_ADDR:
            fprintf(stream, " %%%u, %%%u", instruction->as.index_addr.base, instruction->as.index_addr.index);
            break;
        case COG_IR_OP_MAKE_STRUCT:
        case COG_IR_OP_MAKE_ARRAY:
            fprintf(stream, " (");
            for (size_t i = 0; i < instruction->as.aggregate.value_count; ++i) {
                if (i) fprintf(stream, ", ");
                fprintf(stream, "%%%u", instruction->as.aggregate.values[i]);
            }
            fprintf(stream, ")");
            break;
        case COG_IR_OP_EXTRACT_FIELD:
        case COG_IR_OP_EXTRACT_ELEMENT:
            fprintf(stream, " %%%u, %u", instruction->as.extract.aggregate, instruction->as.extract.index);
            break;
    }
    fprintf(stream, "\n");
}

static void dump_edge(FILE *stream, const CogIrBranchEdge *edge)
{
    fprintf(stream, "bb%u(", edge->target);
    for (size_t i = 0; i < edge->argument_count; ++i) {
        if (i) fprintf(stream, ", ");
        fprintf(stream, "%%%u", edge->arguments[i]);
    }
    fprintf(stream, ")");
}

static void dump_terminator(FILE *stream, const CogIrTerminator *term)
{
    fprintf(stream, "    ");
    switch (term->kind) {
        case COG_IR_TERMINATOR_NONE: fprintf(stream, "<no-terminator>"); break;
        case COG_IR_TERMINATOR_BR:
            fprintf(stream, "br "); dump_edge(stream, &term->as.branch.edge); break;
        case COG_IR_TERMINATOR_COND_BR:
            fprintf(stream, "cond_br %%%u, ", term->as.cond_branch.condition);
            dump_edge(stream, &term->as.cond_branch.if_true);
            fprintf(stream, ", ");
            dump_edge(stream, &term->as.cond_branch.if_false);
            break;
        case COG_IR_TERMINATOR_SWITCH:
            fprintf(stream, "switch %%%u", term->as.switch_term.value);
            for (size_t i = 0; i < term->as.switch_term.case_count; ++i) {
                fprintf(stream, ", @c%u -> ", term->as.switch_term.cases[i].key);
                dump_edge(stream, &term->as.switch_term.cases[i].edge);
            }
            fprintf(stream, ", default -> ");
            dump_edge(stream, &term->as.switch_term.default_edge);
            break;
        case COG_IR_TERMINATOR_RET:
            fprintf(stream, "ret");
            if (term->as.ret.has_value) fprintf(stream, " %%%u", term->as.ret.value);
            break;
        case COG_IR_TERMINATOR_TRAP:
            fprintf(stream, "trap %d", (int)term->as.trap.reason); break;
        case COG_IR_TERMINATOR_UNREACHABLE:
            fprintf(stream, "unreachable"); break;
    }
    fprintf(stream, "\n");
}

void cog_ir_dump(FILE *stream, const CogIrModule *module)
{
    if (!stream || !module)
        return;

    fprintf(stream, "module target(pointer=%u)\n", module->target.pointer_bits);
    dump_type_declarations(stream, module);

    for (size_t i = 0; i < module->global_count; ++i) {
        const CogIrGlobal *global = &module->globals[i];
        fprintf(stream, "global @g%zu", i);
        if (!string_view_is_empty(global->debug_name)) {
            fprintf(stream, " \""); dump_sv(stream, global->debug_name); fprintf(stream, "\"");
        }
        fprintf(stream, " : "); dump_type_ref(stream, module, global->type);
        if (global->abi_type != COG_IR_ABI_TYPE_INVALID)
            fprintf(stream, " abi=@a%u", global->abi_type);
        fprintf(stream, " = @c%u(", global->static_initializer);
        dump_constant(stream, module, global->static_initializer);
        fprintf(stream, ")\n");
    }

    for (size_t i = 0; i < module->function_count; ++i) {
        const CogIrFunction *function = &module->functions[i];
        const CogIrType *type = cog_ir_get_type(module, function->type);
        if (function->id == module->init_function)
            fprintf(stream, "init ");
        else if (function->id == module->entry_function)
            fprintf(stream, "entry ");
        else
            fprintf(stream, "%s ", function->kind == COG_IR_FUNCTION_DECLARATION ? "declare" : "func");
        fprintf(stream, "@f%zu", i);
        if (!string_view_is_empty(function->debug_name)) {
            fprintf(stream, " \""); dump_sv(stream, function->debug_name); fprintf(stream, "\"");
        }
        fprintf(stream, "(");
        for (size_t p = 0; p < function->parameter_count; ++p) {
            if (p) fprintf(stream, ", ");
            fprintf(stream, "%%%u: ", function->parameters[p]);
            dump_type_ref(stream, module, type->as.function.parameter_types[p]);
        }
        if (type->as.function.is_variadic) {
            if (function->parameter_count)
                fprintf(stream, ", ");
            fprintf(stream, "...");
        }
        fprintf(stream, ") -> ");
        dump_type_ref(stream, module, type->as.function.result_type);
        fprintf(stream, " [abi=%s, linkage=%s", abi_name(function->abi.abi), linkage_name(function->linkage));
        fprintf(stream, "]");

        if (function->kind == COG_IR_FUNCTION_DECLARATION) {
            fprintf(stream, "\n");
            continue;
        }

        fprintf(stream, " {\n");
        if (function->slot_count) {
            fprintf(stream, "  slots:\n");
            for (size_t s = 0; s < function->slot_count; ++s) {
                fprintf(stream, "    $s%zu : ", s);
                dump_type_ref(stream, module, function->slots[s].type);
                if (!string_view_is_empty(function->slots[s].debug_name)) {
                    fprintf(stream, " \""); dump_sv(stream, function->slots[s].debug_name); fprintf(stream, "\"");
                }
                fprintf(stream, "\n");
            }
        }

        for (size_t b = 0; b < function->block_count; ++b) {
            const CogIrBlock *block = &function->blocks[b];
            fprintf(stream, "  bb%zu", b);
            if (!string_view_is_empty(block->debug_name)) {
                fprintf(stream, " \""); dump_sv(stream, block->debug_name); fprintf(stream, "\"");
            }
            fprintf(stream, "(");
            for (size_t p = 0; p < block->parameter_count; ++p) {
                if (p) fprintf(stream, ", ");
                fprintf(stream, "%%%u: ", block->parameters[p].value);
                dump_type_ref(stream, module, block->parameters[p].type);
            }
            fprintf(stream, "):\n");
            for (size_t in = 0; in < block->instruction_count; ++in)
                dump_instruction(stream, module, &block->instructions[in]);
            dump_terminator(stream, &block->terminator);
        }
        fprintf(stream, "}\n");
    }
}
