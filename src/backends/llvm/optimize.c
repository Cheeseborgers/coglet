#include "backend_llvm_internal.h"

#include <stdio.h>

#include <llvm-c/Error.h>
#include <llvm-c/Transforms/PassBuilder.h>

static const char *optimization_pipeline(CogOptimizationLevel level)
{
    switch (level) {
        case COG_OPTIMIZATION_LEVEL_1: return "default<O1>";
        case COG_OPTIMIZATION_LEVEL_2: return "default<O2>";
        case COG_OPTIMIZATION_LEVEL_3: return "default<O3>";
        case COG_OPTIMIZATION_LEVEL_0: return NULL;
    }
    return NULL;
}

int llvm_backend_optimize_module(LlvmBackend *backend)
{
    if (!backend || !backend->module || !backend->target_machine)
        return 0;

    const char *pipeline = optimization_pipeline(backend->optimization_level);
    if (!pipeline)
        return backend->optimization_level == COG_OPTIMIZATION_LEVEL_0;

    LLVMPassBuilderOptionsRef options = LLVMCreatePassBuilderOptions();
    if (!options) {
        llvm_backend_error(backend, "could not create LLVM pass-builder options");
        return 0;
    }

    LLVMErrorRef error = LLVMRunPasses(
        backend->module,
        pipeline,
        backend->target_machine,
        options
    );
    LLVMDisposePassBuilderOptions(options);

    if (error) {
        char *message = LLVMGetErrorMessage(error);
        fprintf(
            stderr,
            "LLVM backend optimization error (%s): %s\n",
            pipeline,
            message ? message : "unknown pass-manager error"
        );
        if (message)
            LLVMDisposeErrorMessage(message);
        backend->had_error = 1;
        return 0;
    }

    return 1;
}
