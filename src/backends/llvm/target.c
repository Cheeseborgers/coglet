#include "backend_llvm_internal.h"

#include <stdio.h>

static LLVMCodeGenOptLevel codegen_optimization_level(CogOptimizationLevel level)
{
    switch (level) {
        case COG_OPTIMIZATION_LEVEL_0: return LLVMCodeGenLevelNone;
        case COG_OPTIMIZATION_LEVEL_1: return LLVMCodeGenLevelLess;
        case COG_OPTIMIZATION_LEVEL_2: return LLVMCodeGenLevelDefault;
        case COG_OPTIMIZATION_LEVEL_3: return LLVMCodeGenLevelAggressive;
    }
    return LLVMCodeGenLevelNone;
}

static int fail_target_message(LlvmBackend *backend, const char *prefix, char *message)
{
    if (message) {
        fprintf(stderr, "LLVM backend error: %s: %s\n", prefix, message);
        LLVMDisposeMessage(message);
    } else {
        fprintf(stderr, "LLVM backend error: %s\n", prefix);
    }
    backend->had_error = 1;
    return 0;
}

int llvm_backend_init_native_target(LlvmBackend *backend)
{
    if (!backend || !backend->module || !backend->ir)
        return 0;

    TargetInfo host_target = target_info_host();
    if (!target_info_equal(&backend->ir->target, &host_target)) {
        llvm_backend_error(backend, "Stage 3 LLVM emission currently supports only the compiler host target");
        return 0;
    }

    if (LLVMInitializeNativeTarget()) {
        llvm_backend_error(backend, "could not initialize LLVM native target");
        return 0;
    }
    char *triple = LLVMGetDefaultTargetTriple();
    if (!triple) {
        llvm_backend_error(backend, "could not determine LLVM native target triple");
        return 0;
    }

    LLVMTargetRef target = NULL;
    char *target_error = NULL;
    if (LLVMGetTargetFromTriple(triple, &target, &target_error)) {
        LLVMDisposeMessage(triple);
        return fail_target_message(backend, "could not resolve LLVM native target", target_error);
    }
    if (target_error)
        LLVMDisposeMessage(target_error);

    backend->target_machine = LLVMCreateTargetMachine(
        target,
        triple,
        "",
        "",
        codegen_optimization_level(backend->optimization_level),
        LLVMRelocDefault,
        LLVMCodeModelDefault
    );
    if (!backend->target_machine) {
        LLVMDisposeMessage(triple);
        llvm_backend_error(backend, "could not create LLVM native target machine");
        return 0;
    }

    backend->target_data = LLVMCreateTargetDataLayout(backend->target_machine);
    if (!backend->target_data) {
        LLVMDisposeMessage(triple);
        llvm_backend_error(backend, "could not create LLVM native data layout");
        return 0;
    }

    unsigned pointer_bits = LLVMPointerSize(backend->target_data) * 8u;
    if (pointer_bits != backend->ir->target.pointer_bits) {
        fprintf(
            stderr,
            "LLVM backend error: native LLVM target pointer width is %u bits, but CogIR requires %u bits\n",
            pointer_bits,
            backend->ir->target.pointer_bits
        );
        backend->had_error = 1;
        LLVMDisposeMessage(triple);
        return 0;
    }

    LLVMSetTarget(backend->module, triple);
    LLVMSetModuleDataLayout(backend->module, backend->target_data);
    LLVMDisposeMessage(triple);
    return 1;
}

int llvm_backend_init_native_asm_printer(LlvmBackend *backend)
{
    if (LLVMInitializeNativeAsmPrinter()) {
        llvm_backend_error(backend, "could not initialize LLVM native asm printer");
        return 0;
    }
    return 1;
}

void llvm_backend_dispose_target(LlvmBackend *backend)
{
    if (!backend)
        return;
    if (backend->target_data) {
        LLVMDisposeTargetData(backend->target_data);
        backend->target_data = NULL;
    }
    if (backend->target_machine) {
        LLVMDisposeTargetMachine(backend->target_machine);
        backend->target_machine = NULL;
    }
}
