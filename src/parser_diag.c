#include "parser_diag.h"

#include <stdio.h>

#include "parser.h"

/**
 * @brief Prints the source line containing a token.
 *
 * Searches backwards to the beginning of the line and forwards to the
 * end of the line before writing it to stderr.
 *
 * @param source Complete source code.
 * @param tok Token whose source line should be printed.
 */
static void print_source_line(const char *source, Token const *tok)
{
    const char *line = tok->start;

    while (line > source && line[-1] != '\n')
        line--;

    const char *end = line;

    while (*end && *end != '\n')
        end++;

    fwrite(line, 1, end - line, stderr);
    fputc('\n', stderr);
}

/**
 * @brief Prints a caret marker beneath a token.
 *
 * Outputs whitespace matching the token's column followed by a '^'
 * character and '~' characters spanning the token's length.
 *
 * @param source Complete source code.
 * @param tok Token to highlight.
 */
static void print_caret( const char *source, Token const *tok)
{
    const char *line = tok->start;

    while (line > source && line[-1] != '\n')
        line--;

    for (const char *p = line; p < tok->start; p++)
    {
        if (*p == '\t')
            fputc('\t', stderr);
        else
            fputc(' ', stderr);
    }

    int length = tok->length > 0 ? tok->length : 1;

    fputc('^', stderr);

    for (int i = 1; i < length; i++)
        fputc('~', stderr);

    fputc('\n', stderr);
}

/**
 * @brief Prints a formatted parser diagnostic.
 *
 * Produces output similar to modern compiler diagnostics:
 *
 * @code
 * example.c:4:10: error: expected ';'
 *     let x = 42
 *              ^
 * @endcode
 *
 * @param filename Name of the source file.
 * @param source Complete source code.
 * @param d Diagnostic to print.
 */
void parser_print_diagnostic( const char *filename, const char *source, ParserDiagnostic const *d)
{
    fprintf(
        stderr,
        "%s:%d:%d: error: %s\n",
        filename,
        d->token.line,
        d->token.column,
        d->message
    );

    print_source_line(source, &d->token);
    print_caret(source, &d->token);
}

/**
 * @brief Prints all diagnostics collected by a parser.
 *
 * Diagnostics are printed in the order they were produced during
 * parsing.
 *
 * @param filename Name of the source file.
 * @param source Complete source code.
 * @param parser Parser containing diagnostics.
 */
void parser_print_diagnostics(const char *filename, const char *source, const Parser *parser) {

    for (const ParserDiagnosticNode *node = parser->diagnostics_first; node; node = node->next) {
        parser_print_diagnostic(
            filename,
            source,
            &node->diagnostic
        );
}
}
