// src/coglet.c

#include <stdio.h>
#include <string.h>

#include "backend_c.h"
#include "compiler_driver.h"
#include "cog_ir_lower.h"

static void print_usage(const char *program)
{
    fprintf(
        stderr,
        "usage: %s <file> [-o <executable>] [--emit-c <file>] [-L <dir>|-L<dir>] [-l <name>|-l<name>]\n",
        program
    );
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage(argv[0]);
        return COMPILE_STATUS_DRIVER_ERROR;
    }

    const char *input_path = argv[1];
    const char *output_path = NULL;
    const char *emit_c_path = NULL;

    /* argc is an upper bound for each repeated option category. */
    const char *library_dirs[argc];
    const char *libraries[argc];
    int library_dir_count = 0;
    int library_count = 0;

    for (int i = 2; i < argc; i++) {
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

        fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
        print_usage(argv[0]);
        return COMPILE_STATUS_DRIVER_ERROR;
    }

    if (!output_path && (library_dir_count > 0 || library_count > 0)) {
        fprintf(stderr, "error: -L/-l linker options require -o <executable>\n");
        return COMPILE_STATUS_DRIVER_ERROR;
    }

    CompileResult result;
    CompileStatus status = compile_parse_and_check(input_path, &result);

    if (status != COMPILE_STATUS_OK) {
        int exit_code = status_to_exit_code(status);
        compile_result_destroy(&result);
        return exit_code;
    }

    int exit_code = 0;

    if (!emit_c_path && !output_path) {
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

    if (emit_c_path) {
        CBackendStatus backend_status = c_backend_emit_file(
            emit_c_path,
            &ir
        );

        if (backend_status != C_BACKEND_STATUS_OK)
            exit_code = 3;
    }

    if (exit_code == 0 && output_path) {
        CBackendLinkOptions link_options = {
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
    }

    cog_ir_module_destroy(&ir);
    arena_destroy(ir_diag_arena);
    return exit_code;
}
