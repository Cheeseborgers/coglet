#ifndef COGLET_DIAGNOSTIC_H
#define COGLET_DIAGNOSTIC_H

#include <stddef.h>
#include <stdio.h>

#include "source.h"
#include "utils/arena.h"

typedef enum DiagnosticSeverity {
    DIAGNOSTIC_ERROR,
    DIAGNOSTIC_WARNING,
    DIAGNOSTIC_NOTE,
} DiagnosticSeverity;

typedef enum DiagnosticPhase {
    DIAGNOSTIC_PHASE_LEXER,
    DIAGNOSTIC_PHASE_PARSER,
    DIAGNOSTIC_PHASE_SEMANTIC,
    DIAGNOSTIC_PHASE_IR,
    DIAGNOSTIC_PHASE_BACKEND,
} DiagnosticPhase;

typedef struct Diagnostic {
    DiagnosticSeverity severity;
    DiagnosticPhase phase;
    SourceSpan span;
    const char *message;

    struct Diagnostic *next;
} Diagnostic;

typedef struct DiagnosticList {
    Arena *arena;
    Diagnostic *first;
    Diagnostic *last;
    size_t count;
} DiagnosticList;

void diagnostic_list_init(DiagnosticList *list, Arena *arena);

void diagnostic_add(
    DiagnosticList *list,
    DiagnosticSeverity severity,
    DiagnosticPhase phase,
    SourceSpan span,
    const char *message
);

void diagnostic_add_fmt(
    DiagnosticList *list,
    DiagnosticSeverity severity,
    DiagnosticPhase phase,
    SourceSpan span,
    const char *fmt,
    ...
);

const char *diagnostic_severity_name(DiagnosticSeverity severity);

void diagnostic_print(
    FILE *stream,
    const SourceManager *sources,
    const Diagnostic *diagnostic
);

void diagnostic_print_all(
    FILE *stream,
    const SourceManager *sources,
    const DiagnosticList *diagnostics
);

#endif
