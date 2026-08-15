// src/coglet.c

#include <stdio.h>
#include <string.h>

#include "backend_c.h"
#include "compiler_driver.h"

static void print_usage(const char *program)
{
    fprintf(
        stderr,
        "usage: %s <file> [-o <executable>] [--emit-c <file>]\n",
        program
    );
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage(argv[0]);
        return COMPILE_STATUS_DRIVER_ERROR;
    }

    const char *input_path = argv[1];
    const char *output_path = NULL;
    const char *emit_c_path = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc || output_path) {
                print_usage(argv[0]);
                return COMPILE_STATUS_DRIVER_ERROR;
            }
            output_path = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "--emit-c") == 0) {
            if (i + 1 >= argc || emit_c_path) {
                print_usage(argv[0]);
                return COMPILE_STATUS_DRIVER_ERROR;
            }
            emit_c_path = argv[++i];
            continue;
        }

        fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
        print_usage(argv[0]);
        return COMPILE_STATUS_DRIVER_ERROR;
    }

    CompileResult result;
    CompileStatus status = compile_parse_and_check(input_path, &result);

    if (status != COMPILE_STATUS_OK) {
        int exit_code = status_to_exit_code(status);
        compile_result_destroy(&result);
        return exit_code;
    }

    int exit_code = 0;

    if (emit_c_path) {
        CBackendStatus backend_status = c_backend_emit_file(
            emit_c_path,
            result.filename,
            result.program,
            &result.sem
        );

        if (backend_status != C_BACKEND_STATUS_OK)
            exit_code = 3;
    }

    if (exit_code == 0 && output_path) {
        CBackendStatus backend_status = c_backend_build_executable(
            output_path,
            result.filename,
            result.program,
            &result.sem
        );

        if (backend_status != C_BACKEND_STATUS_OK)
            exit_code = 3;
    }

    compile_result_destroy(&result);
    return exit_code;
}
