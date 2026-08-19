#ifndef COGLET_PARSER_DIAG_H
#define COGLET_PARSER_DIAG_H

#include "diagnostic.h"

typedef struct Parser Parser;

/* Prints the parser's collected diagnostics through its SourceManager. */
void parser_print_diagnostics(const Parser *parser);

#endif
