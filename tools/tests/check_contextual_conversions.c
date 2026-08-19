#include <stdio.h>
#include <string.h>

#include "compiler_driver.h"

typedef struct ConversionCounts {
    int int_materialize;
    int int_to_float;
    int float_materialize;
    int null_to_pointer;
    int pointer_qualification;
    int c_string_to_pointer;
    int slice_qualification;
    int array_to_slice;
} ConversionCounts;

static int fail(const char *message) {
    fprintf(stderr, "contextual-conversion check failed: %s\n", message);
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return 1;
    }

    TargetConfig target_config = target_config_native();
    CompileResult result;
    CompileStatus status = compile_parse_and_check(argv[1], &target_config, &result);

    if (status != COMPILE_STATUS_OK) {
        compile_result_destroy(&result);
        return fail("fixture did not pass semantic analysis");
    }

    ConversionCounts counts = {0};

    for (SemExprInfo *info = result.sem.expr_infos; info; info = info->next) {
        if (info->contextual_conversion == SEM_CONTEXT_CONVERSION_NONE) {
            if (info->contextual_type) {
                compile_result_destroy(&result);
                return fail("contextual type recorded without a conversion kind");
            }
            continue;
        }

        if (!info->contextual_type ||
            semantic_get_effective_expr_type(&result.sem, info->node) != info->contextual_type) {
            compile_result_destroy(&result);
            return fail("effective expression type does not match contextual destination");
        }

        switch (info->contextual_conversion) {
            case SEM_CONTEXT_CONVERSION_NONE:
                break;

            case SEM_CONTEXT_CONVERSION_INT_MATERIALIZE:
                counts.int_materialize++;
                break;

            case SEM_CONTEXT_CONVERSION_INT_TO_FLOAT_MATERIALIZE:
                counts.int_to_float++;
                break;

            case SEM_CONTEXT_CONVERSION_FLOAT_MATERIALIZE:
                counts.float_materialize++;
                break;

            case SEM_CONTEXT_CONVERSION_NULL_TO_POINTER:
                counts.null_to_pointer++;
                break;

            case SEM_CONTEXT_CONVERSION_POINTER_QUALIFICATION:
                counts.pointer_qualification++;
                break;

            case SEM_CONTEXT_CONVERSION_C_STRING_TO_POINTER:
                counts.c_string_to_pointer++;
                break;

            case SEM_CONTEXT_CONVERSION_SLICE_QUALIFICATION:
                counts.slice_qualification++;
                break;

            case SEM_CONTEXT_CONVERSION_ARRAY_TO_SLICE:
                counts.array_to_slice++;
                break;
        }
    }

#define REQUIRE_COUNT(field, description) \
    do { \
        if (counts.field <= 0) { \
            compile_result_destroy(&result); \
            return fail("missing " description " conversion metadata"); \
        } \
    } while (0)

    REQUIRE_COUNT(int_materialize, "integer materialization");
    REQUIRE_COUNT(int_to_float, "integer-to-float materialization");
    REQUIRE_COUNT(float_materialize, "float materialization");
    REQUIRE_COUNT(null_to_pointer, "null-to-pointer");
    REQUIRE_COUNT(pointer_qualification, "pointer qualification");
    REQUIRE_COUNT(c_string_to_pointer, "C-string-to-pointer");
    REQUIRE_COUNT(slice_qualification, "slice qualification");
    REQUIRE_COUNT(array_to_slice, "array-to-slice");

#undef REQUIRE_COUNT

    printf(
        "contextual conversions recorded: int=%d int->float=%d float=%d "
        "null=%d pointer-qual=%d c-string=%d slice-qual=%d array-to-slice=%d\n",
        counts.int_materialize,
        counts.int_to_float,
        counts.float_materialize,
        counts.null_to_pointer,
        counts.pointer_qualification,
        counts.c_string_to_pointer,
        counts.slice_qualification,
        counts.array_to_slice
    );

    compile_result_destroy(&result);
    return 0;
}
