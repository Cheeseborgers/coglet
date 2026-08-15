#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diagnostic.h"
#include "parser.h"
#include "source.h"
#include "utils/arena.h"

static int fail(const char *message)
{
    fprintf(stderr, "source provenance test failed: %s\n", message);
    return 1;
}

int main(void)
{
    Arena *arena = arena_create(4096);
    Arena *scratch = arena_create(2048);

    SourceManager sources;
    source_manager_init(&sources, arena);

    const char *first_source = "first::() -> void { return; }\n";
    const char *second_source = "\nsecond::() -> void { return; }\n";

    SourceFileId first_id = source_manager_add(&sources, "first.cog", first_source);
    SourceFileId second_id = source_manager_add(&sources, "second.cog", second_source);

    if (first_id == second_id)
        return fail("source file IDs are not unique");

    Parser first_parser;
    parser_init_with_source(&first_parser, &sources, first_id, arena, scratch);
    Node *first_program = parse_program(&first_parser);

    Parser second_parser;
    parser_init_with_source(&second_parser, &sources, second_id, arena, scratch);
    Node *second_program = parse_program(&second_parser);

    if (first_parser.had_error || second_parser.had_error)
        return fail("valid source failed to parse");

    if (!first_program || first_program->as.program.statements.count != 1 ||
        !second_program || second_program->as.program.statements.count != 1) {
        return fail("unexpected parsed program shape");
    }

    Node *first_decl = first_program->as.program.statements.items[0];
    Node *second_decl = second_program->as.program.statements.items[0];

    if (first_decl->span.file_id != first_id ||
        second_decl->span.file_id != second_id) {
        return fail("AST nodes lost their source file identity");
    }

    if (first_decl->span.line != 1 || first_decl->span.column != 1 ||
        second_decl->span.line != 2 || second_decl->span.column != 1) {
        return fail("AST start positions are incorrect");
    }

    const SourceFile *first_file = source_manager_get(&sources, first_id);
    const SourceFile *second_file = source_manager_get(&sources, second_id);

    if (!first_file || !second_file ||
        strcmp(first_file->filename, "first.cog") != 0 ||
        strcmp(second_file->filename, "second.cog") != 0) {
        return fail("source manager lookup returned the wrong file");
    }

    DiagnosticList diagnostics;
    diagnostic_list_init(&diagnostics, arena);
    diagnostic_add(
        &diagnostics,
        DIAGNOSTIC_ERROR,
        DIAGNOSTIC_PHASE_SEMANTIC,
        second_decl->span,
        "example diagnostic"
    );

    FILE *capture = tmpfile();
    if (!capture)
        return fail("could not create diagnostic capture file");

    diagnostic_print_all(capture, &sources, &diagnostics);
    fflush(capture);
    rewind(capture);

    char buffer[512];
    size_t length = fread(buffer, 1, sizeof(buffer) - 1, capture);
    buffer[length] = '\0';
    fclose(capture);

    const char *expected =
        "second.cog:2:1: error: example diagnostic\n"
        "second::() -> void { return; }\n"
        "^~~~~~\n";

    if (strcmp(buffer, expected) != 0) {
        fprintf(stderr, "expected:\n%sactual:\n%s", expected, buffer);
        return fail("multi-file diagnostic rendering is incorrect");
    }

    arena_destroy(scratch);
    arena_destroy(arena);
    return 0;
}
