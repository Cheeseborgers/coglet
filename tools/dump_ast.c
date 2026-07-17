// Reads a file, runs it through lexer+parser ONLY, prints the AST.
// Stage 2 test harness: lets us verify parsing is correct in
// isolation, same idea as dump_tokens.c for the le
#include <stdio.h>
#include <stdlib.h>

#include "../include/ast.h"
#include "../include/parser.h"
#include "../include/utils/arena.h"
#include "ast/ast_print.h"
#include "utils/utils.h"

int main(int argc, char **argv) {

    if (argc != 2) {
        fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return 1;
    }
    const char *filename = argv[1];

    char *source = read_file(filename);

    Arena *arena   = arena_create(1 << 16);
    Arena *scratch = arena_create(1 << 8);

    Parser parser;
    parser_init(&parser, filename, source, arena, scratch);
    Node *program = parse_program(&parser);

    if (parser.had_error) {
        arena_destroy(arena);
        free(source);
        return 1;
    }

    ast_pretty_print(program);

    arena_destroy(scratch);
    arena_destroy(arena);
    free(source);
    return 0;
}
