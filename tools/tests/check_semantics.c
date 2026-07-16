// tools/tests/check_semantics.c

#include <stdio.h>
#include <stdlib.h>

#include "parser.h"
#include "semantic_anal.h"
#include "diag.h"
#include "utils/arena.h"
#include "utils/utils.h"

int main(int argc, char **argv)
{
    int exit_code = EXIT_OK;

    char *source = NULL;
    Arena *arena = NULL;
    Arena *scratch = NULL;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return EXIT_DRIVER_ERROR;
    }

    const char *filename = argv[1];

    source = read_file(filename);

    if (!source) {
        fprintf(stderr, "error: could not read '%s'\n", filename);
        return EXIT_DRIVER_ERROR;
    }

    arena = arena_create(1 << 16);
    scratch = arena_create(1 << 8);

    if (!arena || !scratch) {
        fprintf(stderr, "error: failed to create compiler arenas\n");
        exit_code = EXIT_DRIVER_ERROR;
        goto cleanup;
    }

    Parser parser;

    parser_init(
        &parser,
        filename,
        source,
        arena,
        scratch
    );

    Node *program = parse_program(&parser);

    if (parser.had_error) {
        for (int i = 0; i < parser.diagnostic_count; i++) {
            print_diagnostic(
                parser.lexer.filename,
                source,
                &parser.diagnostics[i]
            );
        }

        fprintf(
            stderr,
            "%d parser error%s generated.\n",
            parser.error_count,
            parser.error_count == 1 ? "" : "s"
        );

        exit_code = EXIT_PARSE_ERROR;
        goto cleanup;
    }

    SemanticContext sem = {0};
    sem.arena = arena;

    semantic_check(program, &sem);

    if (sem.had_error) {
        /*
         * semantic_error() currently appears to print each individual
         * diagnostic as it is generated, so this only prints the summary.
         */
        fprintf(
            stderr,
            "%d semantic error%s generated.\n",
            sem.error_count,
            sem.error_count == 1 ? "" : "s"
        );

        exit_code = EXIT_SEMANTIC_ERROR;
        goto cleanup;
    }

cleanup:
    if (scratch)
        arena_destroy(scratch);

    if (arena)
        arena_destroy(arena);

    free(source);

    return exit_code;
}
