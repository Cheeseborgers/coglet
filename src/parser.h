#ifndef COG_PARSER_H
#define COG_PARSER_H

#include "lexer.h"
#include "ast.h"
#include "diag.h"
#include "utils/arena.h"

typedef struct {
    Lexer lexer;

    Token current;   // next token not yet consumed (1-token lookahead)
    Token previous;  // last token we consumed

    Arena *arena;

    int had_error;
    int error_count;

    Diagnostic *diagnostics;
    int diagnostic_count;
    int diagnostic_capacity;
} Parser;

void parser_init(Parser *p, const char *filename, const char *source, Arena *arena);

// Parses the whole file into a NODE_PROGRAM.
// Syntax errors are stored in the parser diagnostics list.
// The returned tree may contain NODE_ERROR nodes.
Node *parse_program(Parser *p);

#endif