#include "backends/llvm/backend_llvm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>

typedef struct LlvmFunctionState {
    LLVMValueRef *values;
    LLVMTypeRef *callable_types;
    LLVMValueRef *slots;
    LLVMBasicBlockRef *blocks;
} LlvmFunctionState;

typedef struct LlvmBackend {
    const CogIrModule *ir;
    LLVMContextRef context;
    LLVMModuleRef module;
    LLVMBuilderRef builder;
    LLVMTypeRef *types;
    LLVMValueRef *globals;
    LLVMValueRef *functions;
    LLVMTypeRef *function_types;
    int had_error;
} LlvmBackend;

static void backend_error(LlvmBackend *backend, const char *message)
{
    fprintf(stderr, "LLVM backend error: %s\n", message);
    backend->had_error = 1;
}

static void backend_unsupported_op(LlvmBackend *backend, CogIrOp op)
{
    fprintf(stderr, "LLVM backend error: unsupported CogIR operation '%s' in Stage 1\n", cog_ir_op_name(op));
    backend->had_error = 1;
}

static LLVMTypeRef lower_type(LlvmBackend *backend, CogIrTypeId id);

static LLVMTypeRef lower_function_type(LlvmBackend *backend, const CogIrType *type)
{
    if (!type || type->kind != COG_IR_TYPE_FUNCTION)
        return NULL;
    if (type->as.function.abi != COG_IR_ABI_COGLET || type->as.function.is_variadic) {
        backend_error(backend, "Stage 1 supports only non-variadic Coglet ABI functions");
        return NULL;
    }

    LLVMTypeRef result = lower_type(backend, type->as.function.result_type);
    if (!result)
        return NULL;

    LLVMTypeRef *params = NULL;
    if (type->as.function.parameter_count) {
        params = calloc(type->as.function.parameter_count, sizeof(*params));
        if (!params) {
            backend_error(backend, "out of memory lowering function type");
            return NULL;
        }
        for (size_t i = 0; i < type->as.function.parameter_count; ++i) {
            params[i] = lower_type(backend, type->as.function.parameter_types[i]);
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

static LLVMTypeRef lower_type(LlvmBackend *backend, CogIrTypeId id)
{
    if (id == COG_IR_TYPE_INVALID || (size_t)id >= backend->ir->type_count) {
        backend_error(backend, "invalid CogIR type id");
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
        case COG_IR_TYPE_FUNCTION:
            result = lower_function_type(backend, type);
            break;
        case COG_IR_TYPE_FLOAT:
        case COG_IR_TYPE_ARRAY:
        case COG_IR_TYPE_STRUCT:
        case COG_IR_TYPE_UNION:
        case COG_IR_TYPE_ENUM:
            backend_error(backend, "CogIR type is outside the LLVM Stage 1 subset");
            return NULL;
    }
    backend->types[id] = result;
    return result;
}

static LLVMValueRef lower_constant(LlvmBackend *backend, CogIrConstId id)
{
    const CogIrConstant *constant = cog_ir_get_constant(backend->ir, id);
    if (!constant) {
        backend_error(backend, "invalid CogIR constant id");
        return NULL;
    }
    LLVMTypeRef type = lower_type(backend, constant->type);
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
        case COG_IR_CONST_FLOAT32:
        case COG_IR_CONST_FLOAT64:
        case COG_IR_CONST_ARRAY:
        case COG_IR_CONST_STRUCT:
            backend_error(backend, "CogIR constant is outside the LLVM Stage 1 subset");
            return NULL;
    }
    return NULL;
}

static int allocate_backend_tables(LlvmBackend *backend)
{
    backend->types = calloc(backend->ir->type_count, sizeof(*backend->types));
    backend->globals = calloc(backend->ir->global_count, sizeof(*backend->globals));
    backend->functions = calloc(backend->ir->function_count, sizeof(*backend->functions));
    backend->function_types = calloc(backend->ir->function_count, sizeof(*backend->function_types));
    if ((backend->ir->type_count && !backend->types) ||
        (backend->ir->global_count && !backend->globals) ||
        (backend->ir->function_count && (!backend->functions || !backend->function_types))) {
        backend_error(backend, "out of memory initializing LLVM backend");
        return 0;
    }
    return 1;
}

static void free_backend_tables(LlvmBackend *backend)
{
    free(backend->types);
    free(backend->globals);
    free(backend->functions);
    free(backend->function_types);
}

static int declare_globals(LlvmBackend *backend)
{
    for (size_t i = 0; i < backend->ir->global_count; ++i) {
        const CogIrGlobal *global = &backend->ir->globals[i];
        LLVMTypeRef type = lower_type(backend, global->type);
        if (!type)
            return 0;
        char name[64];
        snprintf(name, sizeof(name), "cog.global.%zu", i);
        LLVMValueRef value = LLVMAddGlobal(backend->module, type, name);
        LLVMSetLinkage(value, global->linkage == COG_IR_LINKAGE_INTERNAL ? LLVMInternalLinkage : LLVMExternalLinkage);
        LLVMSetGlobalConstant(value, global->is_readonly ? 1 : 0);
        if (global->static_initializer != COG_IR_CONST_INVALID) {
            LLVMValueRef init = lower_constant(backend, global->static_initializer);
            if (!init)
                return 0;
            LLVMSetInitializer(value, init);
        }
        backend->globals[i] = value;
    }
    return 1;
}

static int declare_functions(LlvmBackend *backend)
{
    for (size_t i = 0; i < backend->ir->function_count; ++i) {
        const CogIrFunction *function = &backend->ir->functions[i];
        const CogIrType *type = cog_ir_get_type(backend->ir, function->type);
        if (!type || type->kind != COG_IR_TYPE_FUNCTION) {
            backend_error(backend, "function references a non-function CogIR type");
            return 0;
        }
        if (function->abi.abi != COG_IR_ABI_COGLET || function->abi.is_variadic || function->kind != COG_IR_FUNCTION_DEFINITION) {
            backend_error(backend, "Stage 1 supports only defined non-variadic Coglet ABI functions");
            return 0;
        }
        LLVMTypeRef llvm_type = lower_function_type(backend, type);
        if (!llvm_type)
            return 0;
        char name[64];
        snprintf(name, sizeof(name), "cog.fn.%zu", i);
        LLVMValueRef value = LLVMAddFunction(backend->module, name, llvm_type);
        LLVMSetLinkage(value, function->linkage == COG_IR_LINKAGE_INTERNAL ? LLVMInternalLinkage : LLVMExternalLinkage);
        backend->functions[i] = value;
        backend->function_types[i] = llvm_type;
    }
    return 1;
}

static int init_function_state(LlvmBackend *backend, const CogIrFunction *function, LlvmFunctionState *state)
{
    memset(state, 0, sizeof(*state));
    state->values = calloc(function->value_count, sizeof(*state->values));
    state->callable_types = calloc(function->value_count, sizeof(*state->callable_types));
    state->slots = calloc(function->slot_count, sizeof(*state->slots));
    state->blocks = calloc(function->block_count, sizeof(*state->blocks));
    if ((function->value_count && (!state->values || !state->callable_types)) ||
        (function->slot_count && !state->slots) ||
        (function->block_count && !state->blocks)) {
        backend_error(backend, "out of memory lowering LLVM function");
        return 0;
    }
    return 1;
}

static void destroy_function_state(LlvmFunctionState *state)
{
    free(state->values);
    free(state->callable_types);
    free(state->slots);
    free(state->blocks);
}

static int add_edge_incoming(LlvmBackend *backend, const CogIrFunction *function, LlvmFunctionState *state, CogIrBlockId from, const CogIrBranchEdge *edge)
{
    const CogIrBlock *target = cog_ir_get_block(function, edge->target);
    if (!target || edge->argument_count != target->parameter_count) {
        backend_error(backend, "invalid branch edge while lowering LLVM CFG");
        return 0;
    }
    for (size_t i = 0; i < edge->argument_count; ++i) {
        LLVMValueRef incoming = state->values[edge->arguments[i]];
        LLVMValueRef phi = state->values[target->parameters[i].value];
        LLVMBasicBlockRef incoming_block = state->blocks[from];
        if (!incoming || !phi) {
            backend_error(backend, "branch edge references unavailable LLVM value");
            return 0;
        }
        LLVMAddIncoming(phi, &incoming, &incoming_block, 1);
    }
    return 1;
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
            result = lower_constant(backend, insn->as.constant.constant);
            break;
        case COG_IR_OP_FUNCTION_REF: {
            CogIrFunctionId id = insn->as.function_ref.function;
            if ((size_t)id >= backend->ir->function_count) {
                backend_error(backend, "invalid function reference");
                return 0;
            }
            result = backend->functions[id];
            if (insn->result != COG_IR_VALUE_INVALID)
                state->callable_types[insn->result] = backend->function_types[id];
            break;
        }
        case COG_IR_OP_LOCAL_ADDR:
            result = state->slots[insn->as.local_addr.slot];
            break;
        case COG_IR_OP_GLOBAL_ADDR:
            result = backend->globals[insn->as.global_addr.global];
            break;
        case COG_IR_OP_LOAD: {
            const CogIrValue *value = cog_ir_get_value(function, insn->result);
            LLVMTypeRef type = value ? lower_type(backend, value->type) : NULL;
            LLVMValueRef address = state->values[insn->as.load.address];
            if (!type || !address) return 0;
            result = LLVMBuildLoad2(backend->builder, type, address, "");
            if (insn->as.load.is_volatile) LLVMSetVolatile(result, 1);
            break;
        }
        case COG_IR_OP_STORE: {
            LLVMValueRef address = state->values[insn->as.store.address];
            LLVMValueRef value = state->values[insn->as.store.value];
            if (!address || !value) { backend_error(backend, "store references unavailable LLVM value"); return 0; }
            result = LLVMBuildStore(backend->builder, value, address);
            if (insn->as.store.is_volatile) LLVMSetVolatile(result, 1);
            break;
        }
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
        case COG_IR_OP_CALL: {
            LLVMValueRef callee = state->values[insn->as.call.callee];
            LLVMTypeRef callable = state->callable_types[insn->as.call.callee];
            if (!callee || !callable) {
                backend_error(backend, "Stage 1 supports direct calls from function_ref values only");
                return 0;
            }
            LLVMValueRef *args = NULL;
            if (insn->as.call.argument_count) {
                args = calloc(insn->as.call.argument_count, sizeof(*args));
                if (!args) { backend_error(backend, "out of memory lowering call"); return 0; }
                for (size_t i = 0; i < insn->as.call.argument_count; ++i)
                    args[i] = state->values[insn->as.call.arguments[i]];
            }
            result = LLVMBuildCall2(backend->builder, callable, callee, args, (unsigned)insn->as.call.argument_count, "");
            free(args);
            break;
        }
        case COG_IR_OP_FIELD_ADDR: case COG_IR_OP_ARRAY_ELEM_ADDR: case COG_IR_OP_PTR_INDEX_ADDR:
        case COG_IR_OP_MAKE_STRUCT: case COG_IR_OP_MAKE_ARRAY: case COG_IR_OP_EXTRACT_FIELD: case COG_IR_OP_EXTRACT_ELEMENT:
        case COG_IR_OP_IADD_CHECKED: case COG_IR_OP_ISUB_CHECKED: case COG_IR_OP_IMUL_CHECKED: case COG_IR_OP_IDIV_CHECKED:
        case COG_IR_OP_IREM_CHECKED: case COG_IR_OP_INEG_CHECKED: case COG_IR_OP_IADD_WRAP: case COG_IR_OP_ISUB_WRAP:
        case COG_IR_OP_IMUL_WRAP: case COG_IR_OP_INEG_WRAP: case COG_IR_OP_BIT_AND: case COG_IR_OP_BIT_OR:
        case COG_IR_OP_BIT_XOR: case COG_IR_OP_BIT_NOT: case COG_IR_OP_SHL_CHECKED_COUNT: case COG_IR_OP_SHR_SIGNED_CHECKED_COUNT:
        case COG_IR_OP_SHR_UNSIGNED_CHECKED_COUNT: case COG_IR_OP_FADD: case COG_IR_OP_FSUB: case COG_IR_OP_FMUL:
        case COG_IR_OP_FDIV: case COG_IR_OP_FNEG: case COG_IR_OP_FCMP_EQ: case COG_IR_OP_FCMP_NE: case COG_IR_OP_FCMP_LT:
        case COG_IR_OP_FCMP_LE: case COG_IR_OP_FCMP_GT: case COG_IR_OP_FCMP_GE: case COG_IR_OP_PTR_EQ: case COG_IR_OP_PTR_NE:
        case COG_IR_OP_CAST_CHECKED: case COG_IR_OP_INT_TRUNCATE: case COG_IR_OP_PTR_REINTERPRET: case COG_IR_OP_PTR_QUALIFY:
        case COG_IR_OP_C_VARARG_PROMOTE:
            backend_unsupported_op(backend, insn->op);
            return 0;
    }

    if (backend->had_error)
        return 0;
    if (insn->result != COG_IR_VALUE_INVALID)
        state->values[insn->result] = result;
    return 1;
}

static int lower_terminator(LlvmBackend *backend, const CogIrFunction *function, LlvmFunctionState *state, const CogIrBlock *block)
{
    const CogIrTerminator *term = &block->terminator;
    switch (term->kind) {
        case COG_IR_TERMINATOR_BR:
            if (!add_edge_incoming(backend, function, state, block->id, &term->as.branch.edge)) return 0;
            LLVMBuildBr(backend->builder, state->blocks[term->as.branch.edge.target]);
            return 1;
        case COG_IR_TERMINATOR_COND_BR:
            if (!add_edge_incoming(backend, function, state, block->id, &term->as.cond_branch.if_true) ||
                !add_edge_incoming(backend, function, state, block->id, &term->as.cond_branch.if_false)) return 0;
            LLVMBuildCondBr(backend->builder, state->values[term->as.cond_branch.condition],
                            state->blocks[term->as.cond_branch.if_true.target], state->blocks[term->as.cond_branch.if_false.target]);
            return 1;
        case COG_IR_TERMINATOR_RET:
            if (term->as.ret.has_value) LLVMBuildRet(backend->builder, state->values[term->as.ret.value]);
            else LLVMBuildRetVoid(backend->builder);
            return 1;
        case COG_IR_TERMINATOR_UNREACHABLE:
            LLVMBuildUnreachable(backend->builder);
            return 1;
        case COG_IR_TERMINATOR_SWITCH:
            backend_error(backend, "switch terminators are outside the LLVM Stage 1 subset");
            return 0;
        case COG_IR_TERMINATOR_TRAP:
            backend_error(backend, "trap terminators are outside the LLVM Stage 1 subset");
            return 0;
        case COG_IR_TERMINATOR_NONE:
            backend_error(backend, "CogIR block has no terminator");
            return 0;
    }
    return 0;
}

static int lower_function_body(LlvmBackend *backend, const CogIrFunction *function)
{
    LlvmFunctionState state;
    if (!init_function_state(backend, function, &state)) {
        destroy_function_state(&state);
        return 0;
    }

    LLVMValueRef llvm_function = backend->functions[function->id];
    for (size_t i = 0; i < function->parameter_count; ++i)
        state.values[function->parameters[i]] = LLVMGetParam(llvm_function, (unsigned)i);

    for (size_t i = 0; i < function->block_count; ++i) {
        char name[64]; snprintf(name, sizeof(name), "bb.%zu", i);
        state.blocks[i] = LLVMAppendBasicBlockInContext(backend->context, llvm_function, name);
    }

    for (size_t i = 0; i < function->block_count; ++i) {
        const CogIrBlock *block = &function->blocks[i];
        LLVMPositionBuilderAtEnd(backend->builder, state.blocks[i]);
        for (size_t p = 0; p < block->parameter_count; ++p) {
            LLVMTypeRef type = lower_type(backend, block->parameters[p].type);
            if (!type) goto fail;
            state.values[block->parameters[p].value] = LLVMBuildPhi(backend->builder, type, "");
        }
    }

    LLVMPositionBuilderAtEnd(backend->builder, state.blocks[function->entry_block]);
    for (size_t i = 0; i < function->slot_count; ++i) {
        LLVMTypeRef type = lower_type(backend, function->slots[i].type);
        if (!type) goto fail;
        state.slots[i] = LLVMBuildAlloca(backend->builder, type, "");
    }

    for (size_t i = 0; i < function->block_count; ++i) {
        const CogIrBlock *block = &function->blocks[i];
        LLVMPositionBuilderAtEnd(backend->builder, state.blocks[i]);
        for (size_t j = 0; j < block->instruction_count; ++j)
            if (!lower_instruction(backend, function, &state, &block->instructions[j])) goto fail;
        if (!lower_terminator(backend, function, &state, block)) goto fail;
    }

    destroy_function_state(&state);
    return 1;
fail:
    destroy_function_state(&state);
    return 0;
}

static int lower_functions(LlvmBackend *backend)
{
    for (size_t i = 0; i < backend->ir->function_count; ++i)
        if (!lower_function_body(backend, &backend->ir->functions[i])) return 0;
    return 1;
}

static int emit_process_entry(LlvmBackend *backend)
{
    if (backend->ir->entry_function == COG_IR_FUNCTION_INVALID)
        return 1;
    if ((size_t)backend->ir->entry_function >= backend->ir->function_count) {
        backend_error(backend, "invalid resolved executable entry");
        return 0;
    }

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

LlvmBackendStatus llvm_backend_emit_ir_file(const char *output_path, const CogIrModule *module)
{
    if (!output_path || !module) {
        fprintf(stderr, "LLVM backend error: missing output path or CogIR module\n");
        return LLVM_BACKEND_STATUS_UNSUPPORTED;
    }
    if (!cog_ir_module_is_frozen(module)) {
        fprintf(stderr, "LLVM backend error: CogIR module must be frozen before backend emission\n");
        return LLVM_BACKEND_STATUS_UNSUPPORTED;
    }

    LlvmBackend backend;
    memset(&backend, 0, sizeof(backend));
    backend.ir = module;
    backend.context = LLVMContextCreate();
    backend.module = LLVMModuleCreateWithNameInContext("coglet", backend.context);
    backend.builder = LLVMCreateBuilderInContext(backend.context);

    LlvmBackendStatus status = LLVM_BACKEND_STATUS_UNSUPPORTED;
    if (!backend.context || !backend.module || !backend.builder || !allocate_backend_tables(&backend))
        goto cleanup;
    if (!declare_globals(&backend) || !declare_functions(&backend) || !lower_functions(&backend) || !emit_process_entry(&backend))
        goto cleanup;

    char *verify_message = NULL;
    if (LLVMVerifyModule(backend.module, LLVMReturnStatusAction, &verify_message)) {
        fprintf(stderr, "LLVM backend verifier error: %s", verify_message ? verify_message : "invalid LLVM module\n");
        if (verify_message) LLVMDisposeMessage(verify_message);
        status = LLVM_BACKEND_STATUS_INVALID_IR;
        goto cleanup;
    }
    if (verify_message) LLVMDisposeMessage(verify_message);

    char *write_error = NULL;
    if (LLVMPrintModuleToFile(backend.module, output_path, &write_error)) {
        fprintf(stderr, "LLVM backend error: could not write '%s': %s\n", output_path, write_error ? write_error : "unknown error");
        if (write_error) LLVMDisposeMessage(write_error);
        status = LLVM_BACKEND_STATUS_IO_ERROR;
        goto cleanup;
    }
    if (write_error) LLVMDisposeMessage(write_error);
    status = LLVM_BACKEND_STATUS_OK;

cleanup:
    free_backend_tables(&backend);
    if (backend.builder) LLVMDisposeBuilder(backend.builder);
    if (backend.module) LLVMDisposeModule(backend.module);
    if (backend.context) LLVMContextDispose(backend.context);
    return status;
}
