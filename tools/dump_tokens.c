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

    Lexer lx;
    lexer_init(&lx, filename, source);

    for (;;) {
        Token t = lexer_next(&lx);

        if (t.length > 0 &&
            t.type != TOK_EOF &&
            t.type != TOK_ERROR) {
            printf("%d %s %.*s\n",
                   t.line,
                   token_type_name(t.type),
                   t.length,
                   t.start);
            } else {
                printf("%d %s\n",
                       t.line,
                       token_type_name(t.type));
            }

        if (t.type == TOK_EOF || t.type == TOK_ERROR) {
            break;
        }
    }

    free(source);
    return 0;
}
