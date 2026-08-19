// Reads a file through lexer+parser only and prints the AST.
// Used by parser snapshot and diagnostic tests.
#include <stdio.h>
#include <stdlib.h>

#include "ast.h"
#include "parser.h"
#include "parser_diag.h"
#include "utils/arena.h"
#include "ast/ast_print.h"
#include "utils/utils.h"
#include "target/target_config.h"

int main(int argc, char **argv) {

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

    TargetConfig target_config = target_config_native();

    Arena *arena   = arena_create(1 << 16);
    Arena *scratch = arena_create(1 << 8);

    Parser parser;
    parser_init(&parser, filename, source, &target_config, arena, scratch);
    Node *program = parse_program(&parser);

    int exit_code = 0;

    if (parser.had_error) {
        parser_print_diagnostics(&parser);
        exit_code = 1;
    } else {
        ast_pretty_print(program);
    }

    arena_destroy(scratch);
    arena_destroy(arena);
    free(source);

    return exit_code;
}
