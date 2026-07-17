// src/coglet.c

#include <stdio.h>

#include "compiler_driver.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(
            stderr,
            "usage: %s <file>\n",
            argv[0]
        );

        return COMPILE_STATUS_DRIVER_ERROR;
    }

    CompileResult result;

    CompileStatus status =
        compile_parse_and_check(
            argv[1],
            &result
        );

    /*
     * Future lowering/code generation will run here after checking
     * for COMPILE_STATUS_OK and before destroying the result.
     */

    compile_result_destroy(&result);
    return status;
}