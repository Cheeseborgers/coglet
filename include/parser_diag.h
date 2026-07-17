#ifndef COGLET_PARSER_DIAG_H
#define COGLET_PARSER_DIAG_H

#include "lexer.h"

typedef struct Parser Parser;

/**
 * @brief Represents a single parser diagnostic.
 *
 * A parser diagnostic associates an error message with the token that
 * caused or best identifies the parsing error.
 */
typedef struct {
    /** Token associated with the diagnostic. */
    Token token;

    /** Human-readable error message. */
    const char *message;
} ParserDiagnostic;

/**
 * @brief Prints a single parser diagnostic to stderr.
 *
 * The diagnostic is formatted similarly to compiler diagnostics,
 * including the filename, line, column, the source line containing the
 * error, and a caret indicating the offending token.
 *
 * @param filename Name of the source file.
 * @param source Complete source code.
 * @param d Diagnostic to print.
 */
void parser_print_diagnostic(
    const char *filename,
    const char *source,
    const ParserDiagnostic *d
);

/**
 * @brief Prints every diagnostic stored by a parser.
 *
 * Iterates over all parser diagnostics and prints them using
 * parser_print_diagnostic().
 *
 * @param filename Name of the source file.
 * @param source Complete source code.
 * @param parser Parser containing the diagnostics.
 */
void parser_print_diagnostics(
    const char *filename,
    const char *source,
    const Parser *parser
);

#endif
