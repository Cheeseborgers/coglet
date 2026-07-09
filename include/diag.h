#ifndef COGLET_DIAGNOSTIC_H
#define COGLET_DIAGNOSTIC_H

#include "lexer.h"

typedef struct {
    Token token;
    const char *message;
} Diagnostic;

void print_diagnostic(const char *filename, const char *source, Diagnostic const *d);

#endif