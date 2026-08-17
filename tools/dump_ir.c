#include "ir/cog_ir_lower.h"

#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <file.cog>\n", argv[0]);
        return 2;
    }

    CompileResult frontend;
    if (compile_parse_and_check(argv[1], &frontend) != COMPILE_STATUS_OK)
        return 1;

    CogIrModule module;
    cog_ir_module_init(&module, &frontend.target);

    Arena *diag_arena = arena_create(16384);
    if (!diag_arena) {
        compile_result_destroy(&frontend);
        cog_ir_module_destroy(&module);
        return 2;
    }
    DiagnosticList diagnostics;
    diagnostic_list_init(&diagnostics, diag_arena);

    CogIrLowerContext lower = {0};
    int ok = cog_ir_lower_context_init(&lower, &frontend, &module, &diagnostics) &&
             cog_ir_lower_program(&lower) &&
             cog_ir_verify(&module, &diagnostics);

    if (!ok || diagnostics.count) {
        diagnostic_print_all(stderr, &module.sources, &diagnostics);
        cog_ir_lower_context_destroy(&lower);
        compile_result_destroy(&frontend);
        cog_ir_module_destroy(&module);
        arena_destroy(diag_arena);
        return 3;
    }

    cog_ir_module_freeze(&module);
    cog_ir_lower_context_destroy(&lower);
    compile_result_destroy(&frontend);

    DiagnosticList after_frontend;
    diagnostic_list_init(&after_frontend, diag_arena);
    if (!cog_ir_verify(&module, &after_frontend) || after_frontend.count) {
        diagnostic_print_all(stderr, &module.sources, &after_frontend);
        cog_ir_module_destroy(&module);
        arena_destroy(diag_arena);
        return 4;
    }

    cog_ir_dump(stdout, &module);

    cog_ir_module_destroy(&module);
    arena_destroy(diag_arena);
    return 0;
}
