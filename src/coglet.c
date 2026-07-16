// src/coglet.c

#include <stdio.h>
#include <stdlib.h>

#include "parser.h"
#include "semantic_anal.h"
#include "utils/arena.h"
#include "utils/utils.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return 2;
    }

    const char *filename = argv[1];

    char *source = read_file(filename);

    if (!source) {
        fprintf(stderr, "error: could not read '%s'\n", filename);
        return 2;
    }

    Arena *arena   = arena_create(MB(2));
    Arena *scratch = arena_create(MB(1));

    if (!arena || !scratch) {
        fprintf(stderr, "error: failed to create compiler arenas\n");
        free(source);
        if (scratch) arena_destroy(scratch);
        if (arena) arena_destroy(arena);
        return 2;
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

        arena_destroy(scratch);
        arena_destroy(arena);
        free(source);

        return 2;
    }

    SemanticContext sem = {0};
    sem.arena = arena;

    semantic_check(program, &sem);

    if (sem.had_error) {
        fprintf(
            stderr,
            "%d semantic error%s generated.\n",
            sem.error_count,
            sem.error_count == 1 ? "" : "s"
        );

        arena_destroy(scratch);
        arena_destroy(arena);
        free(source);

        return 1;
    }

    arena_destroy(scratch);
    arena_destroy(arena);
    free(source);

    return 0;
}
