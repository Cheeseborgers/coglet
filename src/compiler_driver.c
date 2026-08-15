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
        "%ld parser error%s generated.\n",
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

CompileStatus compile_parse_and_check(const char *filename, CompileResult *out) {
    TargetInfo target = target_info_host();
    return compile_parse_and_check_for_target(filename, &target, out);
}

CompileStatus compile_parse_and_check_for_target(
    const char *filename,
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
    out->filename = filename;

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

    if (!filename) {
        fprintf(stderr, "error: no input file\n");
        return out->status;
    }

    out->source = read_file(filename);

    if (!out->source) {
        fprintf(
            stderr,
            "error: could not read '%s'\n",
            filename
        );

        return out->status;
    }

    /*
     * These are initial block sizes, not hard limits. The arena
     * implementation allocates additional blocks when necessary.
     */
    out->arena   = arena_create(MB(2));
    out->scratch = arena_create(MB(1));

    source_manager_init(&out->sources, out->arena);
    out->primary_source_id = source_manager_add(
        &out->sources,
        filename,
        out->source
    );

    parser_init_with_source(
        &out->parser,
        &out->sources,
        out->primary_source_id,
        out->arena,
        out->scratch
    );

    out->program = parse_program(&out->parser);

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

    free(result->source);

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
