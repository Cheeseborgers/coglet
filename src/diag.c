#include "../include/diag.h"

#include <stdio.h>

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


void print_diagnostic( const char *filename, const char *source, Diagnostic const *d)
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