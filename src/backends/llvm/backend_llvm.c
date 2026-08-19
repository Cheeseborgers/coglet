#include "backends/llvm/backend_llvm.h"
#include "backend_llvm_internal.h"
#include "string_decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>

void llvm_backend_error(LlvmBackend *backend, const char *message)
{
    fprintf(stderr, "LLVM backend error: %s\n", message);
    backend->had_error = 1;
}

static int allocate_backend_tables(LlvmBackend *backend)
{
    backend->types = calloc(backend->ir->type_count, sizeof(*backend->types));
    backend->c_aggregate_inner_types = calloc(backend->ir->type_count, sizeof(*backend->c_aggregate_inner_types));
    backend->c_aggregate_is_wrapped = calloc(backend->ir->type_count, sizeof(*backend->c_aggregate_is_wrapped));
    backend->globals = calloc(backend->ir->global_count, sizeof(*backend->globals));
    backend->functions = calloc(backend->ir->function_count, sizeof(*backend->functions));
    backend->function_types = calloc(backend->ir->function_count, sizeof(*backend->function_types));
    backend->c_function_abis = calloc(backend->ir->function_count, sizeof(*backend->c_function_abis));
    if ((backend->ir->type_count && (!backend->types || !backend->c_aggregate_inner_types || !backend->c_aggregate_is_wrapped)) ||
        (backend->ir->global_count && !backend->globals) ||
        (backend->ir->function_count && (!backend->functions || !backend->function_types || !backend->c_function_abis))) {
        llvm_backend_error(backend, "out of memory initializing LLVM backend");
        return 0;
    }
    return 1;
}

static void free_backend_tables(LlvmBackend *backend)
{
    free(backend->types);
    free(backend->c_aggregate_inner_types);
    free(backend->c_aggregate_is_wrapped);
    free(backend->globals);
    free(backend->functions);
    if (backend->c_function_abis) {
        for (size_t i = 0; i < backend->ir->function_count; ++i)
            llvm_dispose_c_function_abi(backend->c_function_abis[i]);
    }
    free(backend->c_function_abis);
    free(backend->function_types);
}

static int declare_globals(LlvmBackend *backend)
{
    for (size_t i = 0; i < backend->ir->global_count; ++i) {
        const CogIrGlobal *global = &backend->ir->globals[i];
        LLVMTypeRef type = global->abi_type != COG_IR_ABI_TYPE_INVALID
            ? llvm_lower_c_object_type(backend, global->abi_type)
            : llvm_lower_type(backend, global->type);
        if (!type)
            return 0;
        char name[64];
        snprintf(name, sizeof(name), "cog.global.%zu", i);
        LLVMValueRef value = LLVMAddGlobal(backend->module, type, name);
        LLVMSetLinkage(value, global->linkage == COG_IR_LINKAGE_INTERNAL ? LLVMInternalLinkage : LLVMExternalLinkage);
        LLVMSetGlobalConstant(value, global->is_readonly ? 1 : 0);
        if (global->static_initializer != COG_IR_CONST_INVALID) {
            const CogIrConstant *ir_init = cog_ir_get_constant(backend->ir, global->static_initializer);
            LLVMValueRef init = global->abi_type != COG_IR_ABI_TYPE_INVALID && ir_init &&
                ir_init->kind == COG_IR_CONST_ZERO
                ? LLVMConstNull(type)
                : llvm_lower_constant(backend, global->static_initializer);
            if (!init)
                return 0;
            LLVMSetInitializer(value, init);
        }
        backend->globals[i] = value;
        if (!llvm_debug_declare_global(backend, global, value))
            return 0;
    }
    return 1;
}

static int declare_functions(LlvmBackend *backend)
{
    for (size_t i = 0; i < backend->ir->function_count; ++i) {
        const CogIrFunction *function = &backend->ir->functions[i];
        const CogIrType *type = cog_ir_get_type(backend->ir, function->type);
        if (!type || type->kind != COG_IR_TYPE_FUNCTION) {
            llvm_backend_error(backend, "function references a non-function CogIR type");
            return 0;
        }
        LLVMTypeRef llvm_type = NULL;
        if (function->abi.abi == COG_IR_ABI_COGLET) {
            if (function->abi.is_variadic || function->kind != COG_IR_FUNCTION_DEFINITION) {
                llvm_backend_error(backend, "ordinary Coglet functions must be defined and non-variadic in the LLVM backend");
                return 0;
            }
            llvm_type = llvm_lower_function_signature(backend, function->type);
        } else if (function->abi.abi == COG_IR_ABI_C) {
            LlvmCAbiSignature *signature = llvm_build_c_function_abi(backend, function);
            if (!signature)
                return 0;
            backend->c_function_abis[i] = signature;
            llvm_type = llvm_c_function_abi_type(signature);
        } else {
            llvm_backend_error(backend, "unknown CogIR function ABI");
            return 0;
        }
        if (!llvm_type)
            return 0;

        char generated_name[64];
        const char *name = generated_name;
        char *external_name = NULL;
        if (function->abi.abi == COG_IR_ABI_C &&
            function->linkage == COG_IR_LINKAGE_EXTERNAL &&
            function->abi.external_symbol.length) {
            external_name = malloc(function->abi.external_symbol.length + 1);
            if (!external_name) {
                llvm_backend_error(backend, "out of memory copying external C symbol name");
                return 0;
            }
            memcpy(external_name, function->abi.external_symbol.data, function->abi.external_symbol.length);
            external_name[function->abi.external_symbol.length] = '\0';
            name = external_name;
        } else {
            snprintf(generated_name, sizeof(generated_name), "cog.fn.%zu", i);
        }

        LLVMValueRef value = LLVMAddFunction(backend->module, name, llvm_type);
        free(external_name);
        LLVMSetLinkage(value, function->linkage == COG_IR_LINKAGE_INTERNAL ? LLVMInternalLinkage : LLVMExternalLinkage);
        if (function->abi.abi == COG_IR_ABI_C &&
            !llvm_apply_c_function_abi(backend, value, backend->c_function_abis[i])) {
            return 0;
        }
        backend->functions[i] = value;
        backend->function_types[i] = llvm_type;
        if (!llvm_debug_declare_function(backend, function, value))
            return 0;
    }
    return 1;
}

static int init_function_state(LlvmBackend *backend, const CogIrFunction *function, LlvmFunctionState *state)
{
    memset(state, 0, sizeof(*state));
    state->values = calloc(function->value_count, sizeof(*state->values));
    state->slots = calloc(function->slot_count, sizeof(*state->slots));
    state->blocks = calloc(function->block_count, sizeof(*state->blocks));
    if ((function->value_count && !state->values) ||
        (function->slot_count && !state->slots) ||
        (function->block_count && !state->blocks)) {
        llvm_backend_error(backend, "out of memory lowering LLVM function");
        return 0;
    }
    return 1;
}

static void destroy_function_state(LlvmFunctionState *state)
{
    free(state->values);
    free(state->slots);
    free(state->blocks);
}

static LLVMIntPredicate predicate_for_op(CogIrOp op)
{
    switch (op) {
        case COG_IR_OP_ICMP_EQ:  return LLVMIntEQ;
        case COG_IR_OP_ICMP_NE:  return LLVMIntNE;
        case COG_IR_OP_ICMP_SLT: return LLVMIntSLT;
        case COG_IR_OP_ICMP_SLE: return LLVMIntSLE;
        case COG_IR_OP_ICMP_SGT: return LLVMIntSGT;
        case COG_IR_OP_ICMP_SGE: return LLVMIntSGE;
        case COG_IR_OP_ICMP_ULT: return LLVMIntULT;
        case COG_IR_OP_ICMP_ULE: return LLVMIntULE;
        case COG_IR_OP_ICMP_UGT: return LLVMIntUGT;
        case COG_IR_OP_ICMP_UGE: return LLVMIntUGE;
        default: return LLVMIntEQ;
    }
}

static int lower_instruction(LlvmBackend *backend, const CogIrFunction *function, LlvmFunctionState *state, const CogIrInstruction *insn)
{
    LLVMValueRef result = NULL;
    switch (insn->op) {
        case COG_IR_OP_CONST:
            result = llvm_lower_constant(backend, insn->as.constant.constant);
            break;
        case COG_IR_OP_FUNCTION_REF: {
            CogIrFunctionId id = insn->as.function_ref.function;
            if ((size_t)id >= backend->ir->function_count) {
                llvm_backend_error(backend, "invalid function reference");
                return 0;
            }
            result = backend->functions[id];
            break;
        }
        case COG_IR_OP_LOCAL_ADDR:
        case COG_IR_OP_GLOBAL_ADDR:
        case COG_IR_OP_FIELD_ADDR:
        case COG_IR_OP_ARRAY_ELEM_ADDR:
        case COG_IR_OP_PTR_INDEX_ADDR:
        case COG_IR_OP_LOAD:
        case COG_IR_OP_STORE:
        case COG_IR_OP_MAKE_STRUCT:
        case COG_IR_OP_MAKE_ARRAY:
        case COG_IR_OP_EXTRACT_FIELD:
        case COG_IR_OP_EXTRACT_ELEMENT:
        case COG_IR_OP_PTR_EQ:
        case COG_IR_OP_PTR_NE:
        case COG_IR_OP_PTR_REINTERPRET:
        case COG_IR_OP_PTR_QUALIFY:
            if (!llvm_lower_memory_instruction(backend, function, state, insn, &result))
                return 0;
            break;
        case COG_IR_OP_ICMP_EQ: case COG_IR_OP_ICMP_NE:
        case COG_IR_OP_ICMP_SLT: case COG_IR_OP_ICMP_SLE:
        case COG_IR_OP_ICMP_SGT: case COG_IR_OP_ICMP_SGE:
        case COG_IR_OP_ICMP_ULT: case COG_IR_OP_ICMP_ULE:
        case COG_IR_OP_ICMP_UGT: case COG_IR_OP_ICMP_UGE:
            result = LLVMBuildICmp(backend->builder, predicate_for_op(insn->op), state->values[insn->as.binary.lhs], state->values[insn->as.binary.rhs], "");
            break;
        case COG_IR_OP_BOOL_NOT: {
            LLVMValueRef one = LLVMConstInt(LLVMIntTypeInContext(backend->context, 1), 1, 0);
            result = LLVMBuildXor(backend->builder, state->values[insn->as.unary.operand], one, "");
            break;
        }
        case COG_IR_OP_SIZE_OF:
        case COG_IR_OP_ALIGN_OF: {
            LLVMTypeRef queried = llvm_lower_type(
                backend, insn->as.type_query.queried_type);
            LLVMTypeRef result_type = llvm_lower_type(backend, insn->result_type);
            if (!queried || !result_type) {
                llvm_backend_error(backend, "type-layout query references unavailable LLVM type");
                return 0;
            }
            uint64_t value = insn->op == COG_IR_OP_SIZE_OF
                ? LLVMABISizeOfType(backend->target_data, queried)
                : (uint64_t)LLVMABIAlignmentOfType(backend->target_data, queried);
            result = LLVMConstInt(result_type, value, 0);
            break;
        }
        case COG_IR_OP_CALL: {
            LLVMValueRef callee = state->values[insn->as.call.callee];
            const CogIrValue *callee_value = cog_ir_get_value(function, insn->as.call.callee);
            const CogIrType *callee_type = callee_value
                ? cog_ir_get_type(backend->ir, callee_value->type)
                : NULL;
            if (!callee || !callee_type || callee_type->kind != COG_IR_TYPE_FUNCTION) {
                llvm_backend_error(backend, "call references unavailable or non-function CogIR value");
                return 0;
            }

            if (callee_type->as.function.abi == COG_IR_ABI_COGLET) {
                if (callee_type->as.function.is_variadic) {
                    llvm_backend_error(backend, "ordinary Coglet variadic calls are unsupported");
                    return 0;
                }
                LLVMTypeRef callable = llvm_lower_function_signature(backend, callee_value->type);
                if (!callable)
                    return 0;
                LLVMValueRef *args = NULL;
                if (insn->as.call.argument_count) {
                    args = calloc(insn->as.call.argument_count, sizeof(*args));
                    if (!args) {
                        llvm_backend_error(backend, "out of memory lowering call");
                        return 0;
                    }
                    for (size_t i = 0; i < insn->as.call.argument_count; ++i) {
                        args[i] = state->values[insn->as.call.arguments[i]];
                        if (!args[i]) {
                            free(args);
                            llvm_backend_error(backend, "call argument references unavailable LLVM value");
                            return 0;
                        }
                    }
                }
                result = LLVMBuildCall2(
                    backend->builder, callable, callee, args,
                    (unsigned)insn->as.call.argument_count, "");
                free(args);
            } else if (callee_type->as.function.abi == COG_IR_ABI_C) {
                if (callee_value->abi_type == COG_IR_ABI_TYPE_INVALID) {
                    llvm_backend_error(backend, "C ABI call is missing exact function-pointer ABI metadata");
                    return 0;
                }
                LlvmCAbiSignature *signature =
                    llvm_build_c_function_pointer_abi(backend, callee_value->abi_type);
                if (!signature)
                    return 0;
                int ok = llvm_lower_c_call(
                    backend, function, state, insn, callee, signature, &result);
                llvm_dispose_c_function_abi(signature);
                if (!ok)
                    return 0;
            } else {
                llvm_backend_error(backend, "call uses an unknown CogIR function ABI");
                return 0;
            }
            break;
        }
        case COG_IR_OP_IADD_CHECKED: case COG_IR_OP_ISUB_CHECKED: case COG_IR_OP_IMUL_CHECKED:
        case COG_IR_OP_IDIV_CHECKED: case COG_IR_OP_IREM_CHECKED: case COG_IR_OP_INEG_CHECKED:
        case COG_IR_OP_IADD_WRAP: case COG_IR_OP_ISUB_WRAP: case COG_IR_OP_IMUL_WRAP: case COG_IR_OP_INEG_WRAP:
        case COG_IR_OP_BIT_AND: case COG_IR_OP_BIT_OR: case COG_IR_OP_BIT_XOR: case COG_IR_OP_BIT_NOT:
        case COG_IR_OP_SHL_CHECKED_COUNT: case COG_IR_OP_SHR_SIGNED_CHECKED_COUNT: case COG_IR_OP_SHR_UNSIGNED_CHECKED_COUNT:
        case COG_IR_OP_INT_TRUNCATE:
            if (!llvm_lower_integer_instruction(backend, function, state, insn, &result))
                return 0;
            break;
        case COG_IR_OP_CAST_CHECKED: {
            const CogIrValue *operand = cog_ir_get_value(function, insn->as.conversion.operand);
            const CogIrType *source = operand ? cog_ir_get_type(backend->ir, operand->type) : NULL;
            const CogIrType *target = cog_ir_get_type(backend->ir, insn->result_type);
            if (!source || !target) {
                llvm_backend_error(backend, "cast.checked references unavailable CogIR type");
                return 0;
            }
            if (source->kind == COG_IR_TYPE_FLOAT || target->kind == COG_IR_TYPE_FLOAT) {
                if (!llvm_lower_float_instruction(backend, function, state, insn, &result))
                    return 0;
            } else if (!llvm_lower_integer_instruction(backend, function, state, insn, &result)) {
                return 0;
            }
            break;
        }
        case COG_IR_OP_FADD: case COG_IR_OP_FSUB: case COG_IR_OP_FMUL: case COG_IR_OP_FDIV: case COG_IR_OP_FNEG:
        case COG_IR_OP_FCMP_EQ: case COG_IR_OP_FCMP_NE: case COG_IR_OP_FCMP_LT: case COG_IR_OP_FCMP_LE:
        case COG_IR_OP_FCMP_GT: case COG_IR_OP_FCMP_GE:
            if (!llvm_lower_float_instruction(backend, function, state, insn, &result))
                return 0;
            break;
        case COG_IR_OP_ASM: {
            size_t input_count = insn->as.asm_stmt.input_count;
            LLVMValueRef *inputs = input_count
                ? calloc(input_count, sizeof(*inputs))
                : NULL;
            LLVMTypeRef *parameter_types = input_count
                ? calloc(input_count, sizeof(*parameter_types))
                : NULL;
            if (input_count && (!inputs || !parameter_types)) {
                free(inputs);
                free(parameter_types);
                return 0;
            }

            for (size_t i = 0; i < input_count; ++i) {
                inputs[i] = state->values[insn->as.asm_stmt.inputs[i]];
                parameter_types[i] = inputs[i] ? LLVMTypeOf(inputs[i]) : NULL;
                if (!inputs[i] || !parameter_types[i]) {
                    free(inputs);
                    free(parameter_types);
                    llvm_backend_error(backend, "inline asm operand references unavailable LLVM value");
                    return 0;
                }
            }

            LLVMTypeRef result_type = llvm_lower_type(backend, insn->result_type);
            if (!result_type) {
                free(inputs);
                free(parameter_types);
                return 0;
            }

            StringDecodeInfo info = string_analyze(insn->as.asm_stmt.template_text);
            if (!info.ok) {
                free(inputs);
                free(parameter_types);
                llvm_backend_error(backend, "inline asm template contains an invalid escape sequence");
                return 0;
            }
            char *asm_text = calloc((size_t)info.decoded_length + 1, 1);
            if (!asm_text) {
                free(inputs);
                free(parameter_types);
                return 0;
            }
            if (info.decoded_length && !string_decode_into(
                    insn->as.asm_stmt.template_text,
                    asm_text).ok) {
                free(asm_text);
                free(inputs);
                free(parameter_types);
                llvm_backend_error(backend, "failed to decode inline asm template");
                return 0;
            }

            size_t constraint_length = insn->as.asm_stmt.output_constraint.length;
            for (size_t i = 0; i < input_count; ++i)
                constraint_length += 1 + insn->as.asm_stmt.input_constraints[i].length;
            char *constraints = calloc(constraint_length + 1, 1);
            if (!constraints) {
                free(asm_text);
                free(inputs);
                free(parameter_types);
                return 0;
            }
            size_t constraint_offset = 0;
            memcpy(
                constraints + constraint_offset,
                insn->as.asm_stmt.output_constraint.data,
                insn->as.asm_stmt.output_constraint.length
            );
            constraint_offset += insn->as.asm_stmt.output_constraint.length;
            for (size_t i = 0; i < input_count; ++i) {
                constraints[constraint_offset++] = ',';
                memcpy(
                    constraints + constraint_offset,
                    insn->as.asm_stmt.input_constraints[i].data,
                    insn->as.asm_stmt.input_constraints[i].length
                );
                constraint_offset += insn->as.asm_stmt.input_constraints[i].length;
            }

            LLVMTypeRef function_type = LLVMFunctionType(
                result_type,
                parameter_types,
                (unsigned)input_count,
                0
            );
            LLVMValueRef inline_asm = LLVMGetInlineAsm(
                function_type,
                asm_text,
                (size_t)info.decoded_length,
                constraints,
                constraint_length,
                insn->as.asm_stmt.is_volatile,
                0,
                LLVMInlineAsmDialectATT,
                0
            );
            free(constraints);
            free(asm_text);
            free(parameter_types);
            if (!inline_asm) {
                free(inputs);
                llvm_backend_error(backend, "failed to construct LLVM inline asm");
                return 0;
            }

            result = LLVMBuildCall2(
                backend->builder,
                function_type,
                inline_asm,
                inputs,
                (unsigned)input_count,
                "asm"
            );
            free(inputs);
            if (!result)
                return 0;
            break;
        }
        case COG_IR_OP_C_VARARG_PROMOTE:
            if (!llvm_lower_c_vararg_promotion(backend, function, state, insn, &result))
                return 0;
            break;
    }

    if (backend->had_error)
        return 0;
    if (insn->result != COG_IR_VALUE_INVALID)
        state->values[insn->result] = result;
    return 1;
}

static int lower_function_body(LlvmBackend *backend, const CogIrFunction *function)
{
    LlvmFunctionState state;
    if (!init_function_state(backend, function, &state)) {
        destroy_function_state(&state);
        return 0;
    }

    LLVMValueRef llvm_function = backend->functions[function->id];
    state.function = llvm_function;
    state.c_abi = function->abi.abi == COG_IR_ABI_C
        ? backend->c_function_abis[function->id]
        : NULL;

    for (size_t i = 0; i < function->block_count; ++i) {
        char name[64]; snprintf(name, sizeof(name), "bb.%zu", i);
        state.blocks[i] = LLVMAppendBasicBlockInContext(backend->context, llvm_function, name);
    }

    llvm_debug_clear_location(backend);
    for (size_t i = 0; i < function->block_count; ++i) {
        const CogIrBlock *block = &function->blocks[i];
        LLVMPositionBuilderAtEnd(backend->builder, state.blocks[i]);
        for (size_t p = 0; p < block->parameter_count; ++p) {
            LLVMTypeRef type = llvm_lower_type(backend, block->parameters[p].type);
            if (!type) goto fail;
            state.values[block->parameters[p].value] = LLVMBuildPhi(backend->builder, type, "");
        }
    }

    LLVMPositionBuilderAtEnd(backend->builder, state.blocks[function->entry_block]);
    if (function->abi.abi == COG_IR_ABI_C) {
        if (!llvm_map_c_function_parameters(backend, function, &state, state.c_abi))
            goto fail;
    } else {
        for (size_t i = 0; i < function->parameter_count; ++i)
            state.values[function->parameters[i]] = LLVMGetParam(llvm_function, (unsigned)i);
    }

    llvm_debug_clear_location(backend);
    for (size_t i = 0; i < function->slot_count; ++i) {
        LLVMTypeRef type = function->slots[i].abi_type != COG_IR_ABI_TYPE_INVALID
            ? llvm_lower_c_object_type(backend, function->slots[i].abi_type)
            : llvm_lower_type(backend, function->slots[i].type);
        if (!type) goto fail;
        state.slots[i] = LLVMBuildAlloca(backend->builder, type, "");
    }
    if (!llvm_debug_declare_slots(backend, function, &state))
        goto fail;

    for (size_t i = 0; i < function->block_count; ++i) {
        const CogIrBlock *block = &function->blocks[i];
        LLVMPositionBuilderAtEnd(backend->builder, state.blocks[i]);
        for (size_t j = 0; j < block->instruction_count; ++j) {
            llvm_debug_set_location(backend, function, block->instructions[j].span);
            if (!lower_instruction(backend, function, &state, &block->instructions[j])) goto fail;
        }
        llvm_debug_set_location(backend, function, block->terminator.span);
        if (!llvm_lower_terminator(backend, function, &state, block)) goto fail;
    }
    llvm_debug_clear_location(backend);

    destroy_function_state(&state);
    return 1;
fail:
    destroy_function_state(&state);
    return 0;
}

static int lower_functions(LlvmBackend *backend)
{
    for (size_t i = 0; i < backend->ir->function_count; ++i) {
        const CogIrFunction *function = &backend->ir->functions[i];
        if (function->kind == COG_IR_FUNCTION_DECLARATION)
            continue;
        if (!lower_function_body(backend, function))
            return 0;
    }
    return 1;
}

static int emit_process_entry(LlvmBackend *backend)
{
    if (backend->ir->entry_function == COG_IR_FUNCTION_INVALID)
        return 1;
    if ((size_t)backend->ir->entry_function >= backend->ir->function_count) {
        llvm_backend_error(backend, "invalid resolved executable entry");
        return 0;
    }

    llvm_debug_clear_location(backend);
    LLVMTypeRef i32 = LLVMIntTypeInContext(backend->context, 32);
    LLVMTypeRef main_type = LLVMFunctionType(i32, NULL, 0, 0);
    LLVMValueRef main_fn = LLVMAddFunction(backend->module, "main", main_type);
    LLVMSetLinkage(main_fn, LLVMExternalLinkage);
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(backend->context, main_fn, "entry");
    LLVMPositionBuilderAtEnd(backend->builder, entry);

    if (backend->ir->init_function != COG_IR_FUNCTION_INVALID) {
        CogIrFunctionId id = backend->ir->init_function;
        LLVMBuildCall2(backend->builder, backend->function_types[id], backend->functions[id], NULL, 0, "");
    }

    CogIrFunctionId id = backend->ir->entry_function;
    LLVMValueRef result = LLVMBuildCall2(backend->builder, backend->function_types[id], backend->functions[id], NULL, 0, "");
    LLVMBuildRet(backend->builder, result);
    return 1;
}

static void dispose_backend(LlvmBackend *backend)
{
    if (!backend)
        return;
    llvm_debug_dispose(backend);
    free_backend_tables(backend);
    llvm_backend_dispose_target(backend);
    if (backend->builder)
        LLVMDisposeBuilder(backend->builder);
    if (backend->module)
        LLVMDisposeModule(backend->module);
    if (backend->context)
        LLVMContextDispose(backend->context);
    memset(backend, 0, sizeof(*backend));
}

static int optimization_level_valid(CogOptimizationLevel level)
{
    return level >= COG_OPTIMIZATION_LEVEL_0 && level <= COG_OPTIMIZATION_LEVEL_3;
}

static LlvmBackendStatus verify_llvm_module(LlvmBackend *backend, const char *phase)
{
    char *verify_message = NULL;
    if (LLVMVerifyModule(backend->module, LLVMReturnStatusAction, &verify_message)) {
        fprintf(
            stderr,
            "LLVM backend verifier error%s%s: %s",
            phase ? " " : "",
            phase ? phase : "",
            verify_message ? verify_message : "invalid LLVM module\n"
        );
        if (verify_message)
            LLVMDisposeMessage(verify_message);
        return LLVM_BACKEND_STATUS_INVALID_IR;
    }
    if (verify_message)
        LLVMDisposeMessage(verify_message);
    return LLVM_BACKEND_STATUS_OK;
}

static LlvmBackendStatus lower_verified_module(
    const CogIrModule *module,
    const LlvmBackendOptions *options,
    LLVMRelocMode relocation_mode,
    LlvmBackend *backend
) {
    if (!module) {
        fprintf(stderr, "LLVM backend error: missing CogIR module\n");
        return LLVM_BACKEND_STATUS_UNSUPPORTED;
    }
    if (!cog_ir_module_is_frozen(module)) {
        fprintf(stderr, "LLVM backend error: CogIR module must be frozen before backend emission\n");
        return LLVM_BACKEND_STATUS_UNSUPPORTED;
    }

    memset(backend, 0, sizeof(*backend));
    backend->ir = module;
    backend->optimization_level = options
        ? options->optimization_level
        : COG_OPTIMIZATION_LEVEL_0;
    backend->relocation_mode = relocation_mode;
    backend->debug_info = options ? options->debug_info != 0 : 0;
    if (!optimization_level_valid(backend->optimization_level)) {
        fprintf(stderr, "LLVM backend error: invalid optimization level\n");
        return LLVM_BACKEND_STATUS_UNSUPPORTED;
    }
    backend->context = LLVMContextCreate();
    if (!backend->context) {
        fprintf(stderr, "LLVM backend error: could not create LLVM context\n");
        return LLVM_BACKEND_STATUS_CODEGEN_ERROR;
    }

    backend->module = LLVMModuleCreateWithNameInContext("coglet", backend->context);
    if (!backend->module) {
        fprintf(stderr, "LLVM backend error: could not create LLVM module\n");
        return LLVM_BACKEND_STATUS_CODEGEN_ERROR;
    }

    backend->builder = LLVMCreateBuilderInContext(backend->context);
    if (!backend->builder) {
        fprintf(stderr, "LLVM backend error: could not create LLVM builder\n");
        return LLVM_BACKEND_STATUS_CODEGEN_ERROR;
    }

    if (!llvm_backend_init_native_target(backend) ||
        !allocate_backend_tables(backend) ||
        !llvm_debug_init(backend) ||
        !declare_globals(backend) ||
        !declare_functions(backend) ||
        !lower_functions(backend) ||
        !emit_process_entry(backend)) {
        return LLVM_BACKEND_STATUS_UNSUPPORTED;
    }
    llvm_debug_finalize(backend);

    LlvmBackendStatus status = verify_llvm_module(backend, "before optimization");
    if (status != LLVM_BACKEND_STATUS_OK)
        return status;

    if (backend->optimization_level != COG_OPTIMIZATION_LEVEL_0) {
        if (!llvm_backend_optimize_module(backend))
            return LLVM_BACKEND_STATUS_OPTIMIZATION_ERROR;
        status = verify_llvm_module(backend, "after optimization");
        if (status != LLVM_BACKEND_STATUS_OK)
            return status;
    }

    return LLVM_BACKEND_STATUS_OK;
}

LlvmBackendStatus llvm_backend_emit_ir_file(
    const char *output_path,
    const CogIrModule *module,
    const LlvmBackendOptions *options
)
{
    if (!output_path) {
        fprintf(stderr, "LLVM backend error: missing LLVM IR output path\n");
        return LLVM_BACKEND_STATUS_IO_ERROR;
    }

    LlvmBackend backend;
    memset(&backend, 0, sizeof(backend));
    LlvmBackendStatus status = lower_verified_module(
        module, options, LLVMRelocDefault, &backend);
    if (status != LLVM_BACKEND_STATUS_OK)
        goto cleanup;

    char *write_error = NULL;
    if (LLVMPrintModuleToFile(backend.module, output_path, &write_error)) {
        fprintf(
            stderr,
            "LLVM backend error: could not write '%s': %s\n",
            output_path,
            write_error ? write_error : "unknown error"
        );
        if (write_error)
            LLVMDisposeMessage(write_error);
        status = LLVM_BACKEND_STATUS_IO_ERROR;
        goto cleanup;
    }
    if (write_error)
        LLVMDisposeMessage(write_error);
    status = LLVM_BACKEND_STATUS_OK;

cleanup:
    dispose_backend(&backend);
    return status;
}

static LlvmBackendStatus emit_codegen_file_with_relocation(
    const char *output_path,
    const CogIrModule *module,
    const LlvmBackendOptions *options,
    LLVMRelocMode relocation_mode,
    LLVMCodeGenFileType file_type,
    const char *output_kind
)
{
    if (!output_path) {
        fprintf(stderr, "LLVM backend error: missing %s output path\n", output_kind);
        return LLVM_BACKEND_STATUS_IO_ERROR;
    }

    LlvmBackend backend;
    memset(&backend, 0, sizeof(backend));
    LlvmBackendStatus status = lower_verified_module(module, options, relocation_mode, &backend);
    if (status != LLVM_BACKEND_STATUS_OK)
        goto cleanup;
    if (!llvm_backend_init_native_asm_printer(&backend)) {
        status = LLVM_BACKEND_STATUS_CODEGEN_ERROR;
        goto cleanup;
    }

    size_t path_length = strlen(output_path);
    char *mutable_path = malloc(path_length + 1);
    if (!mutable_path) {
        fprintf(
            stderr,
            "LLVM backend error: out of memory copying %s output path\n",
            output_kind
        );
        status = LLVM_BACKEND_STATUS_CODEGEN_ERROR;
        goto cleanup;
    }
    memcpy(mutable_path, output_path, path_length + 1);

    char *emit_error = NULL;
    if (LLVMTargetMachineEmitToFile(
            backend.target_machine,
            backend.module,
            mutable_path,
            file_type,
            &emit_error)) {
        fprintf(
            stderr,
            "LLVM backend error: could not emit native %s '%s': %s\n",
            output_kind,
            output_path,
            emit_error ? emit_error : "unknown target code-generation error"
        );
        if (emit_error)
            LLVMDisposeMessage(emit_error);
        free(mutable_path);
        status = LLVM_BACKEND_STATUS_CODEGEN_ERROR;
        goto cleanup;
    }
    if (emit_error)
        LLVMDisposeMessage(emit_error);
    free(mutable_path);
    status = LLVM_BACKEND_STATUS_OK;

cleanup:
    dispose_backend(&backend);
    return status;
}

LlvmBackendStatus llvm_backend_emit_assembly_file(
    const char *output_path,
    const CogIrModule *module,
    const LlvmBackendOptions *options
)
{
    return emit_codegen_file_with_relocation(
        output_path,
        module,
        options,
        LLVMRelocPIC,
        LLVMAssemblyFile,
        "assembly"
    );
}

LlvmBackendStatus llvm_backend_emit_object_file(
    const char *output_path,
    const CogIrModule *module,
    const LlvmBackendOptions *options
)
{
    return emit_codegen_file_with_relocation(
        output_path,
        module,
        options,
        LLVMRelocDefault,
        LLVMObjectFile,
        "object"
    );
}

LlvmBackendStatus llvm_backend_emit_position_independent_object_file(
    const char *output_path,
    const CogIrModule *module,
    const LlvmBackendOptions *options
)
{
    return emit_codegen_file_with_relocation(
        output_path,
        module,
        options,
        LLVMRelocPIC,
        LLVMObjectFile,
        "object"
    );
}
