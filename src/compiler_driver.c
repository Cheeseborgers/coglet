#include "compiler_driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser_diag.h"
#include "utils/utils.h"

static void report_parser_errors(const CompileResult *result) {

    parser_print_diagnostics(&result->parser);

    fprintf(
        stderr,
        "%zu parser error%s generated.\n",
        result->parser.diagnostic_count,
        result->parser.diagnostic_count == 1 ? "" : "s"
    );
}

static void report_semantic_error_summary(const CompileResult *result) {

    diagnostic_print_all(stderr, &result->sources, &result->sem.diagnostics);

    fprintf(
        stderr,
        "%d semantic error%s generated.\n",
        result->sem.error_count,
        result->sem.error_count == 1 ? "" : "s"
    );
}

static void append_parser_diagnostics(Parser *aggregate, const Parser *parsed)
{
    if (!aggregate || !parsed)
        return;

    aggregate->had_error = aggregate->had_error || parsed->had_error;
    aggregate->diagnostic_count += parsed->diagnostic_count;
    aggregate->diagnostics.count += parsed->diagnostics.count;

    if (!parsed->diagnostics.first)
        return;

    if (aggregate->diagnostics.last) {
        aggregate->diagnostics.last->next = parsed->diagnostics.first;
    } else {
        aggregate->diagnostics.first = parsed->diagnostics.first;
    }

    aggregate->diagnostics.last = parsed->diagnostics.last;
}

CompileStatus compile_parse_and_check(const char *filename, CompileResult *out) {
    const char *filenames[1] = { filename };
    return compile_parse_and_check_files(filenames, 1, out);
}

CompileStatus compile_parse_and_check_files(
    const char *const *filenames,
    size_t filename_count,
    CompileResult *out
) {
    TargetInfo target = target_info_host();
    return compile_parse_and_check_files_for_target(
        filenames,
        filename_count,
        &target,
        out
    );
}

CompileStatus compile_parse_and_check_for_target(
    const char *filename,
    const TargetInfo *target,
    CompileResult *out
) {
    const char *filenames[1] = { filename };
    return compile_parse_and_check_files_for_target(filenames, 1, target, out);
}

CompileStatus compile_parse_and_check_files_for_target(
    const char *const *filenames,
    size_t filename_count,
    const TargetInfo *target,
    CompileResult *out
) {

    if (!out) return COMPILE_STATUS_DRIVER_ERROR;

    /*
     * Makes compile_result_destroy() safe after every normal return.
     *
     * A live result must be destroyed before this function is called
     * on it again.
     */
    memset(out, 0, sizeof(*out));

    out->status = COMPILE_STATUS_DRIVER_ERROR;

    if (!target) {
        fprintf(stderr, "error: no target description\n");
        return out->status;
    }

    char target_error[160];
    if (!target_info_validate(target, target_error, sizeof(target_error))) {
        fprintf(stderr, "error: invalid target description: %s\n", target_error);
        return out->status;
    }

    out->target = *target;

    if (!filenames || filename_count == 0 || !filenames[0]) {
        fprintf(stderr, "error: no input file\n");
        return out->status;
    }

    out->filename = filenames[0];
    out->source_buffers = calloc(filename_count, sizeof(*out->source_buffers));
    if (!out->source_buffers) {
        fprintf(stderr, "error: could not allocate source buffer table\n");
        return out->status;
    }
    out->source_buffer_count = filename_count;

    for (size_t i = 0; i < filename_count; ++i) {
        if (!filenames[i]) {
            fprintf(stderr, "error: null input filename\n");
            return out->status;
        }

        out->source_buffers[i] = read_file(filenames[i]);
        if (!out->source_buffers[i]) {
            fprintf(stderr, "error: could not read '%s'\n", filenames[i]);
            return out->status;
        }
    }
    out->source = out->source_buffers[0];

    /*
     * These are initial block sizes, not hard limits. The arena
     * implementation allocates additional blocks when necessary.
     */
    out->arena   = arena_create(MB(2));
    out->scratch = arena_create(MB(1));
    if (!out->arena || !out->scratch) {
        fprintf(stderr, "error: could not allocate compiler arenas\n");
        return out->status;
    }

    source_manager_init(&out->sources, out->arena);

    SourceFileId *source_ids = arena_alloc(
        out->arena,
        filename_count * sizeof(*source_ids)
    );

    for (size_t i = 0; i < filename_count; ++i) {
        source_ids[i] = source_manager_add(
            &out->sources,
            filenames[i],
            out->source_buffers[i]
        );
    }
    out->primary_source_id = source_ids[0];

    memset(&out->parser, 0, sizeof(out->parser));
    out->parser.arena = out->arena;
    out->parser.scratch = out->scratch;
    out->parser.sources = &out->sources;
    out->parser.source_id = out->primary_source_id;
    diagnostic_list_init(&out->parser.diagnostics, out->arena);

    out->program = NULL;

    for (size_t i = 0; i < filename_count; ++i) {
        Parser parser;
        parser_init_with_source(
            &parser,
            &out->sources,
            source_ids[i],
            out->arena,
            out->scratch
        );

        Node *file_program = parse_program(&parser);
        append_parser_diagnostics(&out->parser, &parser);

        if (!out->program) {
            out->program = ast_new_program(out->arena, file_program->span);
        }

        for (int j = 0; j < file_program->as.program.statements.count; ++j) {
            nodelist_push(
                out->arena,
                &out->program->as.program.statements,
                file_program->as.program.statements.items[j]
            );
        }
    }

    if (out->parser.had_error) {
        report_parser_errors(out);

        out->status = COMPILE_STATUS_PARSE_ERROR;
        return out->status;
    }

    out->sem.arena = out->arena;

    semantic_check(out->program, &out->sem, &out->target, &out->sources);

    if (out->sem.had_error) {
        report_semantic_error_summary(out);

        out->status = COMPILE_STATUS_SEMANTIC_ERROR;
        return out->status;
    }

    out->status = COMPILE_STATUS_OK;
    return out->status;
}

void compile_result_destroy(CompileResult *result)
{
    if (!result) return;

    if (result->scratch) arena_destroy(result->scratch);
    if (result->arena)   arena_destroy(result->arena);

    if (result->source_buffers) {
        for (size_t i = 0; i < result->source_buffer_count; ++i)
            free(result->source_buffers[i]);
        free(result->source_buffers);
    } else {
        free(result->source);
    }

    memset(result, 0, sizeof(*result));
}

int status_to_exit_code(CompileStatus status) {
    switch (status) {
        case COMPILE_STATUS_OK:
            return 0;

        case COMPILE_STATUS_SEMANTIC_ERROR:
            return 1;

        case COMPILE_STATUS_PARSE_ERROR:
        case COMPILE_STATUS_DRIVER_ERROR:
            return 2;
    }

    return 2;
}
