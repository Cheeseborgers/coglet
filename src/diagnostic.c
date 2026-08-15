#include "diagnostic.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void diagnostic_list_init(DiagnosticList *list, Arena *arena)
{
    assert(list);
    assert(arena);

    list->arena = arena;
    list->first = NULL;
    list->last = NULL;
    list->count = 0;
}

void diagnostic_add(
    DiagnosticList *list,
    DiagnosticSeverity severity,
    DiagnosticPhase phase,
    SourceSpan span,
    const char *message
) {
    assert(list);
    assert(list->arena);
    assert(message);

    Diagnostic *diagnostic = arena_new(list->arena, Diagnostic);
    diagnostic->severity = severity;
    diagnostic->phase = phase;
    diagnostic->span = span;
    diagnostic->message = arena_strdup_len(list->arena, message, strlen(message));
    diagnostic->next = NULL;

    if (list->last) {
        list->last->next = diagnostic;
    } else {
        list->first = diagnostic;
    }

    list->last = diagnostic;
    list->count++;
}

void diagnostic_add_fmt(
    DiagnosticList *list,
    DiagnosticSeverity severity,
    DiagnosticPhase phase,
    SourceSpan span,
    const char *fmt,
    ...
) {
    assert(list);
    assert(list->arena);
    assert(fmt);

    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);

    int required = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);

    if (required < 0) {
        va_end(args);
        diagnostic_add(list, severity, phase, span, "diagnostic formatting failed");
        return;
    }

    char *message = arena_alloc(list->arena, (size_t)required + 1);
    vsnprintf(message, (size_t)required + 1, fmt, args);
    va_end(args);

    Diagnostic *diagnostic = arena_new(list->arena, Diagnostic);
    diagnostic->severity = severity;
    diagnostic->phase = phase;
    diagnostic->span = span;
    diagnostic->message = message;
    diagnostic->next = NULL;

    if (list->last) {
        list->last->next = diagnostic;
    } else {
        list->first = diagnostic;
    }

    list->last = diagnostic;
    list->count++;
}

const char *diagnostic_severity_name(DiagnosticSeverity severity)
{
    switch (severity) {
        case DIAGNOSTIC_ERROR: return "error";
        case DIAGNOSTIC_WARNING: return "warning";
        case DIAGNOSTIC_NOTE: return "note";
    }

    return "diagnostic";
}

static void print_source_excerpt(
    FILE *stream,
    const SourceFile *file,
    SourceSpan span
) {
    if (!file || !file->source || !source_span_is_valid(span))
        return;

    size_t start = span.start_offset;
    if (start > file->length)
        start = file->length;

    size_t line_start = start;
    while (line_start > 0 && file->source[line_start - 1] != '\n')
        line_start--;

    size_t line_end = start;
    while (line_end < file->length && file->source[line_end] != '\n')
        line_end++;

    fwrite(file->source + line_start, 1, line_end - line_start, stream);
    fputc('\n', stream);

    for (size_t i = line_start; i < start; i++) {
        fputc(file->source[i] == '\t' ? '\t' : ' ', stream);
    }

    size_t highlight_end = span.end_offset;
    if (highlight_end <= start)
        highlight_end = start + 1;
    if (highlight_end > line_end)
        highlight_end = line_end;

    size_t highlight_length = highlight_end > start
        ? highlight_end - start
        : 1;

    fputc('^', stream);
    for (size_t i = 1; i < highlight_length; i++)
        fputc('~', stream);
    fputc('\n', stream);
}

void diagnostic_print(
    FILE *stream,
    const SourceManager *sources,
    const Diagnostic *diagnostic
) {
    if (!stream || !diagnostic)
        return;

    const SourceFile *file = source_manager_get(sources, diagnostic->span.file_id);
    const char *filename = file ? file->filename : "<unknown>";
    unsigned line = diagnostic->span.line ? diagnostic->span.line : 1;
    unsigned column = diagnostic->span.column ? diagnostic->span.column : 1;

    fprintf(
        stream,
        "%s:%u:%u: %s: %s\n",
        filename,
        line,
        column,
        diagnostic_severity_name(diagnostic->severity),
        diagnostic->message
    );

    print_source_excerpt(stream, file, diagnostic->span);
}

void diagnostic_print_all(
    FILE *stream,
    const SourceManager *sources,
    const DiagnosticList *diagnostics
) {
    if (!diagnostics)
        return;

    for (const Diagnostic *diagnostic = diagnostics->first;
         diagnostic;
         diagnostic = diagnostic->next) {
        diagnostic_print(stream, sources, diagnostic);
    }
}
