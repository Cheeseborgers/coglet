#include "backend_llvm_internal.h"

#include <stdlib.h>
#include <string.h>

int llvm_get_intrinsic(
    LlvmBackend *backend,
    const char *name,
    LLVMTypeRef *overload_types,
    size_t overload_count,
    LLVMValueRef *out_function,
    LLVMTypeRef *out_function_type
) {
    unsigned id = LLVMLookupIntrinsicID(name, strlen(name));
    if (!id) {
        llvm_backend_error(backend, "required LLVM intrinsic is unavailable");
        return 0;
    }

    LLVMTypeRef type = LLVMIntrinsicGetType(
        backend->context,
        id,
        overload_types,
        overload_count
    );
    LLVMValueRef function = LLVMGetIntrinsicDeclaration(
        backend->module,
        id,
        overload_types,
        overload_count
    );
    if (!type || !function) {
        llvm_backend_error(backend, "could not declare required LLVM intrinsic");
        return 0;
    }

    *out_function = function;
    *out_function_type = type;
    return 1;
}

int llvm_emit_trap(LlvmBackend *backend)
{
    LLVMValueRef trap_function = NULL;
    LLVMTypeRef trap_type = NULL;
    if (!llvm_get_intrinsic(backend, "llvm.trap", NULL, 0, &trap_function, &trap_type))
        return 0;
    LLVMBuildCall2(backend->builder, trap_type, trap_function, NULL, 0, "");
    LLVMBuildUnreachable(backend->builder);
    return 1;
}

int llvm_emit_trap_if(
    LlvmBackend *backend,
    LlvmFunctionState *state,
    LLVMValueRef condition
) {
    if (!condition) {
        llvm_backend_error(backend, "missing LLVM trap condition");
        return 0;
    }

    LLVMBasicBlockRef trap_block = LLVMAppendBasicBlockInContext(
        backend->context,
        state->function,
        "cog.trap"
    );
    LLVMBasicBlockRef continue_block = LLVMAppendBasicBlockInContext(
        backend->context,
        state->function,
        "cog.cont"
    );
    if (!trap_block || !continue_block) {
        llvm_backend_error(backend, "could not create LLVM checked-operation blocks");
        return 0;
    }

    LLVMBuildCondBr(backend->builder, condition, trap_block, continue_block);

    LLVMPositionBuilderAtEnd(backend->builder, trap_block);
    if (!llvm_emit_trap(backend))
        return 0;

    LLVMPositionBuilderAtEnd(backend->builder, continue_block);
    return 1;
}

static int add_edge_incoming(
    LlvmBackend *backend,
    const CogIrFunction *function,
    LlvmFunctionState *state,
    const CogIrBranchEdge *edge
) {
    const CogIrBlock *target = cog_ir_get_block(function, edge->target);
    if (!target || edge->argument_count != target->parameter_count) {
        llvm_backend_error(backend, "invalid branch edge while lowering LLVM CFG");
        return 0;
    }
    for (size_t i = 0; i < edge->argument_count; ++i) {
        LLVMValueRef incoming = state->values[edge->arguments[i]];
        LLVMValueRef phi = state->values[target->parameters[i].value];
        LLVMBasicBlockRef incoming_block = LLVMGetInsertBlock(backend->builder);
        if (!incoming || !phi) {
            llvm_backend_error(backend, "branch edge references unavailable LLVM value");
            return 0;
        }
        LLVMAddIncoming(phi, &incoming, &incoming_block, 1);
    }
    return 1;
}

static LLVMBasicBlockRef append_switch_edge_block(
    LlvmBackend *backend,
    LlvmFunctionState *state,
    const char *name
) {
    LLVMBasicBlockRef block = LLVMAppendBasicBlockInContext(
        backend->context,
        state->function,
        name
    );
    if (!block)
        llvm_backend_error(backend, "could not create LLVM switch edge block");
    return block;
}

static int lower_switch_edge(
    LlvmBackend *backend,
    const CogIrFunction *function,
    LlvmFunctionState *state,
    LLVMBasicBlockRef edge_block,
    const CogIrBranchEdge *edge
) {
    LLVMPositionBuilderAtEnd(backend->builder, edge_block);
    if (!add_edge_incoming(backend, function, state, edge))
        return 0;
    LLVMBuildBr(backend->builder, state->blocks[edge->target]);
    return 1;
}

static int lower_switch_terminator(
    LlvmBackend *backend,
    const CogIrFunction *function,
    LlvmFunctionState *state,
    const CogIrTerminator *term
) {
    LLVMValueRef selector = state->values[term->as.switch_term.value];
    if (!selector) {
        llvm_backend_error(backend, "switch references unavailable LLVM selector");
        return 0;
    }

    size_t count = term->as.switch_term.case_count;
    LLVMBasicBlockRef *case_edges = count ? calloc(count, sizeof(*case_edges)) : NULL;
    if (count && !case_edges) {
        llvm_backend_error(backend, "out of memory lowering LLVM switch");
        return 0;
    }

    LLVMBasicBlockRef default_edge = append_switch_edge_block(
        backend, state, "cog.switch.default.edge"
    );
    if (!default_edge) {
        free(case_edges);
        return 0;
    }
    for (size_t i = 0; i < count; ++i) {
        case_edges[i] = append_switch_edge_block(backend, state, "cog.switch.case.edge");
        if (!case_edges[i]) {
            free(case_edges);
            return 0;
        }
    }

    LLVMValueRef switch_inst = LLVMBuildSwitch(
        backend->builder,
        selector,
        default_edge,
        (unsigned)count
    );
    if (!switch_inst) {
        free(case_edges);
        llvm_backend_error(backend, "could not build LLVM switch terminator");
        return 0;
    }

    for (size_t i = 0; i < count; ++i) {
        LLVMValueRef key = llvm_lower_constant(backend, term->as.switch_term.cases[i].key);
        if (!key) {
            free(case_edges);
            return 0;
        }
        LLVMAddCase(switch_inst, key, case_edges[i]);
    }

    for (size_t i = 0; i < count; ++i) {
        if (!lower_switch_edge(
                backend,
                function,
                state,
                case_edges[i],
                &term->as.switch_term.cases[i].edge
            )) {
            free(case_edges);
            return 0;
        }
    }
    if (!lower_switch_edge(
            backend,
            function,
            state,
            default_edge,
            &term->as.switch_term.default_edge
        )) {
        free(case_edges);
        return 0;
    }

    free(case_edges);
    return 1;
}

int llvm_lower_terminator(
    LlvmBackend *backend,
    const CogIrFunction *function,
    LlvmFunctionState *state,
    const CogIrBlock *block
) {
    const CogIrTerminator *term = &block->terminator;
    switch (term->kind) {
        case COG_IR_TERMINATOR_BR:
            if (!add_edge_incoming(backend, function, state, &term->as.branch.edge)) return 0;
            LLVMBuildBr(backend->builder, state->blocks[term->as.branch.edge.target]);
            return 1;
        case COG_IR_TERMINATOR_COND_BR:
            if (!add_edge_incoming(backend, function, state, &term->as.cond_branch.if_true) ||
                !add_edge_incoming(backend, function, state, &term->as.cond_branch.if_false)) return 0;
            LLVMBuildCondBr(
                backend->builder,
                state->values[term->as.cond_branch.condition],
                state->blocks[term->as.cond_branch.if_true.target],
                state->blocks[term->as.cond_branch.if_false.target]
            );
            return 1;
        case COG_IR_TERMINATOR_SWITCH:
            return lower_switch_terminator(backend, function, state, term);
        case COG_IR_TERMINATOR_RET:
            if (state->c_abi) {
                LLVMValueRef value = term->as.ret.has_value
                    ? state->values[term->as.ret.value]
                    : NULL;
                return llvm_lower_c_return(backend, state, term->as.ret.has_value, value);
            }
            if (term->as.ret.has_value) LLVMBuildRet(backend->builder, state->values[term->as.ret.value]);
            else LLVMBuildRetVoid(backend->builder);
            return 1;
        case COG_IR_TERMINATOR_TRAP:
            return llvm_emit_trap(backend);
        case COG_IR_TERMINATOR_UNREACHABLE:
            LLVMBuildUnreachable(backend->builder);
            return 1;
        case COG_IR_TERMINATOR_NONE:
            llvm_backend_error(backend, "CogIR block has no terminator");
            return 0;
    }
    llvm_backend_error(backend, "unknown CogIR terminator kind");
    return 0;
}
