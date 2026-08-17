// src/coglet.c

#include <stdio.h>
#include <string.h>

#include "../include/backends/backend_c/backend_c.h"
#ifdef COGLET_HAS_LLVM_BACKEND
#include "backends/llvm/backend_llvm.h"
#endif
#include "compiler_driver.h"
#include "coglet_version.h"
#include "coglet_paths.h"
#include "ir/cog_ir_lower.h"
#include "optimization.h"

typedef enum ExecutableBackend {
    EXECUTABLE_BACKEND_HOST_C,
    EXECUTABLE_BACKEND_LLVM,
} ExecutableBackend;

static void print_usage(const char *program)
{
    fprintf(
        stderr,
        "usage: %s <file> [<file> ...] [-o <executable>] [--backend <host-c|llvm>] [--emit-c <file>] [--emit-llvm <file>] [--emit-asm <file>] [-O0|-O1|-O2|-O3] [-g] [-I <dir>|-I<dir>] [--stdlib-root <dir>] [-L <dir>|-L<dir>] [-l <name>|-l<name>]\n"
        "       %s --version\n"
        "       %s --print-stdlib-root\n",
        program,
        program,
        program
    );
}

static int parse_optimization_level(const char *value, CogOptimizationLevel *level)
{
    if (strcmp(value, "-O0") == 0) {
        *level = COG_OPTIMIZATION_LEVEL_0;
        return 1;
    }
    if (strcmp(value, "-O1") == 0) {
        *level = COG_OPTIMIZATION_LEVEL_1;
        return 1;
    }
    if (strcmp(value, "-O2") == 0) {
        *level = COG_OPTIMIZATION_LEVEL_2;
        return 1;
    }
    if (strcmp(value, "-O3") == 0) {
        *level = COG_OPTIMIZATION_LEVEL_3;
        return 1;
    }
    return 0;
}


static int cog_string_view_starts_with_cstr(StringView value, const char *prefix)
{
    size_t prefix_length = strlen(prefix);
    return value.length >= prefix_length &&
           memcmp(value.data, prefix, prefix_length) == 0;
}

static int cog_ir_requires_runtime(const CogIrModule *module)
{
    if (!module)
        return 0;

    for (size_t i = 0; i < module->function_count; ++i) {
        const CogIrFunction *function = &module->functions[i];
        if (function->linkage != COG_IR_LINKAGE_EXTERNAL ||
            function->abi.abi != COG_IR_ABI_C) {
            continue;
        }
        if (cog_string_view_starts_with_cstr(function->abi.external_symbol, "coglet_rt_"))
            return 1;
    }
    return 0;
}

static int cog_ir_requires_runtime_math(const CogIrModule *module)
{
    if (!module)
        return 0;

    for (size_t i = 0; i < module->function_count; ++i) {
        const CogIrFunction *function = &module->functions[i];
        if (function->linkage != COG_IR_LINKAGE_EXTERNAL ||
            function->abi.abi != COG_IR_ABI_C) {
            continue;
        }
        if (cog_string_view_starts_with_cstr(function->abi.external_symbol, "coglet_rt_math_"))
            return 1;
    }
    return 0;
}

static int resolve_runtime_source(
    const char *stdlib_root,
    char *buffer,
    size_t buffer_size
) {
    if (!stdlib_root || !stdlib_root[0]) {
        fprintf(stderr, "error: Coglet runtime support requires a standard-library root\n");
        return 0;
    }

    size_t length = strlen(stdlib_root);
    int separator = stdlib_root[length - 1] != '/' && stdlib_root[length - 1] != '\\';
    int written = snprintf(
        buffer,
        buffer_size,
        "%s%sruntime/coglet_runtime.c",
        stdlib_root,
        separator ? "/" : ""
    );
    if (written < 0 || (size_t)written >= buffer_size) {
        fprintf(stderr, "error: Coglet runtime support path is too long\n");
        return 0;
    }

    FILE *file = fopen(buffer, "rb");
    if (!file) {
        fprintf(
            stderr,
            "error: program requires Coglet runtime support, but '%s' could not be opened\n",
            buffer
        );
        return 0;
    }
    fclose(file);
    return 1;
}

static int parse_executable_backend(const char *value, ExecutableBackend *backend)
{
    if (strcmp(value, "host-c") == 0) {
        *backend = EXECUTABLE_BACKEND_HOST_C;
        return 1;
    }
    if (strcmp(value, "llvm") == 0) {
        *backend = EXECUTABLE_BACKEND_LLVM;
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("coglet %s\n", COGLET_VERSION_STRING);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--print-stdlib-root") == 0) {
        printf("%s\n", COGLET_CONFIGURED_STDLIB_ROOT);
        return 0;
    }

    if (argc < 2) {
        print_usage(argv[0]);
        return COMPILE_STATUS_DRIVER_ERROR;
    }

    const char *input_paths[argc];
    size_t input_path_count = 0;
    const char *output_path = NULL;
    const char *emit_c_path = NULL;
    const char *emit_llvm_path = NULL;
    const char *emit_asm_path = NULL;
    ExecutableBackend executable_backend = EXECUTABLE_BACKEND_HOST_C;
    int executable_backend_explicit = 0;
    CogOptimizationLevel optimization_level = COG_OPTIMIZATION_LEVEL_0;
    int debug_info = 0;
    const char *stdlib_root = COGLET_CONFIGURED_STDLIB_ROOT;
    int stdlib_root_explicit = 0;

    /* argc is an upper bound for each repeated option category. */
    const char *module_search_dirs[argc];
    const char *library_dirs[argc];
    const char *libraries[argc];
    size_t module_search_dir_count = 0;
    int library_dir_count = 0;
    int library_count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc || output_path) {
                print_usage(argv[0]);
                return COMPILE_STATUS_DRIVER_ERROR;
            }
            output_path = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "--emit-c") == 0) {
            if (i + 1 >= argc || emit_c_path) {
                print_usage(argv[0]);
                return COMPILE_STATUS_DRIVER_ERROR;
            }
            emit_c_path = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "--emit-llvm") == 0) {
            if (i + 1 >= argc || emit_llvm_path) {
                print_usage(argv[0]);
                return COMPILE_STATUS_DRIVER_ERROR;
            }
            emit_llvm_path = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "--emit-asm") == 0) {
            if (i + 1 >= argc || emit_asm_path) {
                print_usage(argv[0]);
                return COMPILE_STATUS_DRIVER_ERROR;
            }
            emit_asm_path = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "--backend") == 0) {
            if (i + 1 >= argc || executable_backend_explicit ||
                !parse_executable_backend(argv[i + 1], &executable_backend)) {
                print_usage(argv[0]);
                return COMPILE_STATUS_DRIVER_ERROR;
            }
            executable_backend_explicit = 1;
            ++i;
            continue;
        }

        if (strncmp(argv[i], "--backend=", 10) == 0) {
            if (executable_backend_explicit || argv[i][10] == '\0' ||
                !parse_executable_backend(argv[i] + 10, &executable_backend)) {
                print_usage(argv[0]);
                return COMPILE_STATUS_DRIVER_ERROR;
            }
            executable_backend_explicit = 1;
            continue;
        }

        if (strncmp(argv[i], "-O", 2) == 0) {
            if (!parse_optimization_level(argv[i], &optimization_level)) {
                fprintf(stderr, "error: unsupported optimization level '%s'; expected -O0, -O1, -O2, or -O3\n", argv[i]);
                return COMPILE_STATUS_DRIVER_ERROR;
            }
            continue;
        }

        if (strcmp(argv[i], "-g") == 0) {
            debug_info = 1;
            continue;
        }

        if (strcmp(argv[i], "-I") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                fprintf(stderr, "error: -I requires a non-empty module search directory\n");
                print_usage(argv[0]);
                return COMPILE_STATUS_DRIVER_ERROR;
            }
            module_search_dirs[module_search_dir_count++] = argv[++i];
            continue;
        }

        if (strncmp(argv[i], "-I", 2) == 0 && argv[i][2] != '\0') {
            module_search_dirs[module_search_dir_count++] = argv[i] + 2;
            continue;
        }

        if (strcmp(argv[i], "--stdlib-root") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0' || stdlib_root_explicit) {
                fprintf(stderr, "error: --stdlib-root requires one non-empty directory\n");
                print_usage(argv[0]);
                return COMPILE_STATUS_DRIVER_ERROR;
            }
            stdlib_root = argv[++i];
            stdlib_root_explicit = 1;
            continue;
        }

        if (strncmp(argv[i], "--stdlib-root=", 14) == 0) {
            if (argv[i][14] == '\0' || stdlib_root_explicit) {
                fprintf(stderr, "error: --stdlib-root requires one non-empty directory\n");
                print_usage(argv[0]);
                return COMPILE_STATUS_DRIVER_ERROR;
            }
            stdlib_root = argv[i] + 14;
            stdlib_root_explicit = 1;
            continue;
        }

        if (strcmp(argv[i], "-L") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                fprintf(stderr, "error: -L requires a non-empty library directory\n");
                print_usage(argv[0]);
                return COMPILE_STATUS_DRIVER_ERROR;
            }
            library_dirs[library_dir_count++] = argv[++i];
            continue;
        }

        if (strncmp(argv[i], "-L", 2) == 0 && argv[i][2] != '\0') {
            library_dirs[library_dir_count++] = argv[i] + 2;
            continue;
        }

        if (strcmp(argv[i], "-l") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                fprintf(stderr, "error: -l requires a non-empty library name\n");
                print_usage(argv[0]);
                return COMPILE_STATUS_DRIVER_ERROR;
            }
            libraries[library_count++] = argv[++i];
            continue;
        }

        if (strncmp(argv[i], "-l", 2) == 0 && argv[i][2] != '\0') {
            libraries[library_count++] = argv[i] + 2;
            continue;
        }

        if (argv[i][0] != '-') {
            input_paths[input_path_count++] = argv[i];
            continue;
        }

        fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
        print_usage(argv[0]);
        return COMPILE_STATUS_DRIVER_ERROR;
    }

    if (input_path_count == 0) {
        fprintf(stderr, "error: no input file\n");
        print_usage(argv[0]);
        return COMPILE_STATUS_DRIVER_ERROR;
    }

    if (!output_path && (library_dir_count > 0 || library_count > 0)) {
        fprintf(stderr, "error: -L/-l linker options require -o <executable>\n");
        return COMPILE_STATUS_DRIVER_ERROR;
    }
    if (!output_path && executable_backend_explicit) {
        fprintf(stderr, "error: --backend requires -o <executable>\n");
        return COMPILE_STATUS_DRIVER_ERROR;
    }
    if (optimization_level != COG_OPTIMIZATION_LEVEL_0 &&
        output_path && executable_backend == EXECUTABLE_BACKEND_HOST_C) {
        fprintf(
            stderr,
            "error: -O1/-O2/-O3 executable optimization currently requires --backend llvm\n"
        );
        return COMPILE_STATUS_DRIVER_ERROR;
    }
    if (optimization_level != COG_OPTIMIZATION_LEVEL_0 &&
        !emit_llvm_path && !emit_asm_path &&
        (!output_path || executable_backend != EXECUTABLE_BACKEND_LLVM)) {
        fprintf(
            stderr,
            "error: -O1/-O2/-O3 currently require LLVM output via --emit-llvm, --emit-asm, or --backend llvm -o\n"
        );
        return COMPILE_STATUS_DRIVER_ERROR;
    }
    if (debug_info && output_path && executable_backend == EXECUTABLE_BACKEND_HOST_C) {
        fprintf(
            stderr,
            "error: -g executable debug information currently requires --backend llvm\n"
        );
        return COMPILE_STATUS_DRIVER_ERROR;
    }
    if (debug_info &&
        !emit_llvm_path && !emit_asm_path &&
        (!output_path || executable_backend != EXECUTABLE_BACKEND_LLVM)) {
        fprintf(
            stderr,
            "error: -g currently requires LLVM output via --emit-llvm, --emit-asm, or --backend llvm -o\n"
        );
        return COMPILE_STATUS_DRIVER_ERROR;
    }

    CompileOptions compile_options = compile_options_default();
    compile_options.discover_imports = 1;
    compile_options.module_search_dirs = module_search_dirs;
    compile_options.module_search_dir_count = module_search_dir_count;
    compile_options.stdlib_root = stdlib_root;

    CompileResult result;
    CompileStatus status = compile_parse_and_check_files_with_options(
        input_paths,
        input_path_count,
        &compile_options,
        &result
    );

    if (status != COMPILE_STATUS_OK) {
        int exit_code = status_to_exit_code(status);
        compile_result_destroy(&result);
        return exit_code;
    }

    int exit_code = 0;

    if (!emit_c_path && !emit_llvm_path && !emit_asm_path && !output_path) {
        compile_result_destroy(&result);
        return 0;
    }

    /*
     * The backend boundary is CogIR, not frontend state. Lower and verify the
     * complete module while the frontend is alive, freeze it, then destroy the
     * AST/semantic arenas before invoking any backend entry point. This makes
     * accidental backend dependence on Node/Symbol/Type lifetime impossible.
     */
    CogIrModule ir;
    cog_ir_module_init(&ir, &result.target);

    Arena *ir_diag_arena = arena_create(16384);
    if (!ir_diag_arena) {
        fprintf(stderr, "error: could not allocate CogIR diagnostics\n");
        cog_ir_module_destroy(&ir);
        compile_result_destroy(&result);
        return 3;
    }

    DiagnosticList ir_diagnostics;
    diagnostic_list_init(&ir_diagnostics, ir_diag_arena);
    CogIrLowerContext lower = {0};
    int ir_ok = cog_ir_lower_context_init(&lower, &result, &ir, &ir_diagnostics) &&
                cog_ir_lower_program(&lower) &&
                cog_ir_verify(&ir, &ir_diagnostics);

    if (!ir_ok || ir_diagnostics.count) {
        diagnostic_print_all(stderr, &ir.sources, &ir_diagnostics);
        cog_ir_lower_context_destroy(&lower);
        cog_ir_module_destroy(&ir);
        arena_destroy(ir_diag_arena);
        compile_result_destroy(&result);
        return 3;
    }

    cog_ir_module_freeze(&ir);
    cog_ir_lower_context_destroy(&lower);
    compile_result_destroy(&result);

    DiagnosticList frozen_diagnostics;
    diagnostic_list_init(&frozen_diagnostics, ir_diag_arena);
    if (!cog_ir_verify(&ir, &frozen_diagnostics) || frozen_diagnostics.count) {
        diagnostic_print_all(stderr, &ir.sources, &frozen_diagnostics);
        cog_ir_module_destroy(&ir);
        arena_destroy(ir_diag_arena);
        return 3;
    }

    char runtime_source_path[4096];
    const char *runtime_source = NULL;
    int runtime_math = output_path && cog_ir_requires_runtime_math(&ir);
    if (output_path && cog_ir_requires_runtime(&ir)) {
        if (!resolve_runtime_source(stdlib_root, runtime_source_path, sizeof(runtime_source_path))) {
            cog_ir_module_destroy(&ir);
            arena_destroy(ir_diag_arena);
            return 3;
        }
        runtime_source = runtime_source_path;
    }

    if (emit_c_path) {
        CBackendStatus backend_status = c_backend_emit_file(
            emit_c_path,
            &ir
        );

        if (backend_status != C_BACKEND_STATUS_OK)
            exit_code = 3;
    }

    if (exit_code == 0 && emit_llvm_path) {
#ifdef COGLET_HAS_LLVM_BACKEND
        LlvmBackendOptions backend_options = {
            .optimization_level = optimization_level,
            .debug_info = debug_info,
        };
        LlvmBackendStatus backend_status = llvm_backend_emit_ir_file(
            emit_llvm_path,
            &ir,
            &backend_options
        );
        if (backend_status != LLVM_BACKEND_STATUS_OK)
            exit_code = 3;
#else
        fprintf(
            stderr,
            "error: LLVM backend is not available in this build; configure with COGLET_LLVM=ON and LLVM_DIR\n"
        );
        exit_code = 3;
#endif
    }

    if (exit_code == 0 && emit_asm_path) {
#ifdef COGLET_HAS_LLVM_BACKEND
        LlvmBackendOptions backend_options = {
            .optimization_level = optimization_level,
            .debug_info = debug_info,
        };
        LlvmBackendStatus backend_status = llvm_backend_emit_assembly_file(
            emit_asm_path,
            &ir,
            &backend_options
        );
        if (backend_status != LLVM_BACKEND_STATUS_OK)
            exit_code = 3;
#else
        fprintf(
            stderr,
            "error: LLVM backend is not available in this build; configure with COGLET_LLVM=ON and LLVM_DIR\n"
        );
        exit_code = 3;
#endif
    }

    if (exit_code == 0 && output_path) {
        if (executable_backend == EXECUTABLE_BACKEND_HOST_C) {
            CBackendLinkOptions link_options = {
                .runtime_source = runtime_source,
                .runtime_math = runtime_math,
                .library_dirs = library_dirs,
                .library_dir_count = library_dir_count,
                .libraries = libraries,
                .library_count = library_count,
            };

            CBackendStatus backend_status = c_backend_build_executable(
                output_path,
                &ir,
                &link_options
            );

            if (backend_status != C_BACKEND_STATUS_OK)
                exit_code = 3;
        } else {
#ifdef COGLET_HAS_LLVM_BACKEND
            LlvmBackendOptions backend_options = {
                .optimization_level = optimization_level,
                .debug_info = debug_info,
            };
            LlvmBackendLinkOptions link_options = {
                .runtime_source = runtime_source,
                .runtime_math = runtime_math,
                .library_dirs = library_dirs,
                .library_dir_count = library_dir_count,
                .libraries = libraries,
                .library_count = library_count,
            };
            LlvmBackendStatus backend_status = llvm_backend_build_executable(
                output_path,
                &ir,
                &backend_options,
                &link_options
            );
            if (backend_status != LLVM_BACKEND_STATUS_OK)
                exit_code = 3;
#else
            fprintf(
                stderr,
                "error: LLVM backend is not available in this build; configure with COGLET_LLVM=ON and LLVM_DIR\n"
            );
            exit_code = 3;
#endif
        }
    }

    cog_ir_module_destroy(&ir);
    arena_destroy(ir_diag_arena);
    return exit_code;
}
