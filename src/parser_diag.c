#include "parser_diag.h"

#include <stdio.h>

#include "parser.h"

void parser_print_diagnostics(const Parser *parser)
{
    if (!parser)
        return;

    diagnostic_print_all(
        stderr,
        parser->sources,
        &parser->diagnostics
    );
}
