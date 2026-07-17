#ifndef COGLET_PARSER_H
#define COGLET_PARSER_H

#include "lexer.h"
#include "ast.h"
#include "parser_diag.h"
#include "utils/arena.h"

typedef struct Parser {
    Lexer lexer;

    Token current;   // next token not yet consumed (1-token lookahead)
    Token previous;  // last token we consumed

    Arena *arena;
    Arena *scratch;

    int had_error;

    ParserDiagnosticNode *diagnostics_first;
    ParserDiagnosticNode *diagnostics_last;
    size_t diagnostic_count;

    int suppress_struct_init;   // true while parsing a bare if/while/for condition
} Parser;

void parser_init(Parser *p, const char *filename, const char *source, Arena *arena, Arena *scratch);

// Parses the whole file into a NODE_PROGRAM.
// Syntax errors are stored in the parser diagnostics list.
// The returned tree may contain NODE_ERROR nodes.
Node *parse_program(Parser *p);

#endif
