// Reads a file, runs ONLY the lexer, prints every token.
// This is the Stage 1 test harness described in the roadmap:
// it lets us verify lexing is correct before the parser exists.
#include <stdio.h>
#include <stdlib.h>
#include "../include/lexer.h"
#include "../include/utils/utils.h"

int main(int argc, char **argv) {

    if (argc != 2) {
        fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return 1;
    }

    const char *filename = argv[1];

    char *source = read_file(filename);
    if (source == NULL) {
        fprintf(stderr, "error: could not read file '%s'\n", filename);
        return 1;
    }

    Lexer lexer;
    lexer_init(&lexer, filename, source);

    int exit_code = 0;

    for (;;) {
        Token token = lexer_next(&lexer);

        if (token.type == TOK_ERROR) {
            printf(
                "%d %s %.*s\n",
                token.line,
                token_type_name(token.type),
                token.length,
                token.start
            );

            fprintf(
                stderr,
                "%d:%d: error: %s\n",
                token.line,
                token.column,
                lexer.error_msg
                    ? lexer.error_msg
                    : "invalid token"
            );

            exit_code = 1;
            break;
        }

        if (token.type == TOK_EOF) {
            printf(
                "%d %s\n",
                token.line,
                token_type_name(token.type)
            );

            break;
        }

        printf(
            "%d %s %.*s\n",
            token.line,
            token_type_name(token.type),
            token.length,
            token.start
        );
    }

    free(source);
    return exit_code;
}
