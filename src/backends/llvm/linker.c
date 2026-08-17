#include "backend_llvm_internal.h"

#include <stdio.h>

#include "toolchain/native_toolchain.h"

LlvmBackendStatus llvm_backend_build_executable(
    const char *output_path,
    const CogIrModule *module,
    const LlvmBackendOptions *backend_options,
    const LlvmBackendLinkOptions *link_options
) {
    if (!output_path) {
        fprintf(stderr, "LLVM backend error: no executable output path provided\n");
        return LLVM_BACKEND_STATUS_IO_ERROR;
    }

    char object_path[4096];
#if defined(_WIN32)
    const char *object_suffix = ".obj";
#else
    const char *object_suffix = ".o";
#endif
    if (!cog_native_create_temp_file(
            object_path, sizeof(object_path), "coglet-llvm-object", object_suffix)) {
        return LLVM_BACKEND_STATUS_IO_ERROR;
    }

    /*
     * The native compiler driver may default to PIE. Generate executable-link
     * objects as PIC so the LLVM relocation model matches that toolchain policy
     * without teaching CogIR about PIE or platform-specific linker flags.
     */
    LlvmBackendStatus status = llvm_backend_emit_position_independent_object_file(
        object_path,
        module,
        backend_options
    );
    if (status == LLVM_BACKEND_STATUS_OK) {
        CogNativeToolchainLinkOptions native_options = {
            .runtime_source = link_options ? link_options->runtime_source : NULL,
            .runtime_math = link_options ? link_options->runtime_math : 0,
            .library_dirs = link_options ? link_options->library_dirs : NULL,
            .library_dir_count = link_options ? link_options->library_dir_count : 0,
            .libraries = link_options ? link_options->libraries : NULL,
            .library_count = link_options ? link_options->library_count : 0,
        };
        CogNativeToolchainStatus native_status = cog_native_link_object_executable(
            output_path,
            object_path,
            &native_options
        );
        if (native_status != COG_NATIVE_TOOLCHAIN_OK)
            status = LLVM_BACKEND_STATUS_TOOLCHAIN_ERROR;
    }

    cog_native_remove_file(object_path);
    return status;
}
