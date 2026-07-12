/*
 * Main cog compiler dummy test
 *
 */

#include <stdio.h>

#include "parser.h"
#include "../include/ast.h"
#include "../include/diag.h"
#include "../include/semantic_anal.h"

#include "../tools/ast/ast_print.h"
#include "../include/utils/utils.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return 1;
    }

    const char *filename = argv[1];
    char *source = read_file(filename);

    if (!source) {
        fprintf(stderr, "error: could not read '%s'\n", filename);
        return 1;
    }

    Arena *arena   = arena_create(MB(2));
    Arena *scratch = arena_create(MB(1));

    if (!arena || !scratch) {
        fprintf(stderr, "error: failed to create compiler arenas\n");
        free(source);
        return 1;
    }

    Parser parser;
    parser_init(
        &parser,
        filename,
        source,
        arena,
        scratch
    );

    Node *ast = parse_program(&parser);

    int exit_code = 0;

    if (parser.had_error) {
        for (int i = 0;
             i < parser.diagnostic_count;
             i++) {

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

        exit_code = 1;
    } else {
        SemanticContext sem = {0};
        sem.arena = arena;

        semantic_check(ast, &sem);

        if (sem.had_error) {
            fprintf(
                stderr,
                "%d semantic error%s generated.\n",
                sem.error_count,
                sem.error_count == 1 ? "" : "s"
            );

            exit_code = 1;
        } else {
            ast_pretty_print(ast);
        }
    }

    arena_destroy(scratch);
    arena_destroy(arena);
    free(source);

    return exit_code;
}
