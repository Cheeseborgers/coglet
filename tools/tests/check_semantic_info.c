#include <stdio.h>
#include <string.h>

#include "compiler_driver.h"
#include "semantic_info_verifier.h"

static void print_usage(const char *program_name)
{
    fprintf(
        stderr,
        "usage: %s [--dump-semantic-info] <file>\n",
        program_name
    );
}

int main(int argc, char **argv)
{
    int dump_semantic_info = 0;
    const char *input_path = NULL;

    if (argc == 2) {
        input_path = argv[1];
    } else if (
        argc == 3 &&
        strcmp(argv[1], "--dump-semantic-info") == 0
    ) {
        dump_semantic_info = 1;
        input_path = argv[2];
    } else {
        print_usage(argv[0]);
        return 1;
    }

    CompileResult result;
    CompileStatus status = compile_parse_and_check(
        input_path,
        &result
    );

    /*
     * Invalid programs may leave a partially populated semantic-information
     * table. Only inspect that partial state when explicitly requested.
     */
    if (status != COMPILE_STATUS_OK) {
        if (
            dump_semantic_info &&
            status == COMPILE_STATUS_SEMANTIC_ERROR &&
            result.program != NULL
        ) {
            semantic_info_dump_program(
                &result.sem,
                result.program,
                stderr
            );
        }

        compile_result_destroy(&result);
        return 1;
    }

    SemanticInfoVerification verification;

    int verified = semantic_info_verify_program(
        &result.sem,
        result.program,
        stderr,
        &verification
    );

    if (dump_semantic_info) {
        semantic_info_dump_program(
            &result.sem,
            result.program,
            stdout
        );
    }

    if (!verified) {
        compile_result_destroy(&result);
        return 1;
    }

    printf(
        "semantic-info verification passed: "
        "%d expressions, %d mutations, %d table entries\n",
        verification.expression_count,
        verification.mutation_count,
        verification.table_entry_count
    );

    compile_result_destroy(&result);
    return 0;
}
