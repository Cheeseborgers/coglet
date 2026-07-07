/*
 * Main cog compiler dummy test
 *
 */

#include <stdio.h>

#include "parser.h"
#include "ast.h"
#include "diag.h"
#include "semantic_anal.h"

#include "../tools/ast/ast_print.h"
#include "utils/utils.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return 1;
    }

    const char *filename = argv[1];

    char *source = read_file(filename);

    Arena *arena = arena_create(MB(2));
    Parser parser;
    parser_init(&parser, filename, source, arena);

    Node *ast = parse_program(&parser);

    if (!parser.had_error) {
        SemanticContext sem = {0};

        sem.arena = arena;

        semantic_check(ast,&sem);


        if (!sem.had_error)
        {
            ast_pretty_print(ast);
        }
    }
    else {
        for (int i = 0; i < parser.diagnostic_count; i++)
            print_diagnostic(parser.lexer.filename, source, &parser.diagnostics[i]);

        fprintf(stderr, "%d error%s generated.\n",
                parser.error_count,
                parser.error_count == 1 ? "" : "s");

    }

    return 0;
}
