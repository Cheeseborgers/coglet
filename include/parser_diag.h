#ifndef COGLET_PARSER_DIAG_H
#define COGLET_PARSER_DIAG_H

#include "lexer.h"

typedef struct Parser Parser;

typedef struct {
    /** Token associated with the diagnostic. */
    Token token;

    /** Human-readable error message. */
    const char *message;
} ParserDiagnostic;

typedef struct ParserDiagnosticNode {
    ParserDiagnostic diagnostic;
    struct ParserDiagnosticNode *next;
} ParserDiagnosticNode;

void parser_print_diagnostic(
    const char *filename,
    const char *source,
    const ParserDiagnostic *d
);

void parser_print_diagnostics(
    const char *filename,
    const char *source,
    const Parser *parser
);

#endif
