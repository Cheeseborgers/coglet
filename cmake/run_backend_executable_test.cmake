if(NOT DEFINED COMPILER)
    message(FATAL_ERROR "COMPILER is required")
endif()

if(NOT DEFINED INPUT)
    message(FATAL_ERROR "INPUT is required")
endif()

if(NOT DEFINED OUTPUT)
    message(FATAL_ERROR "OUTPUT is required")
endif()

if(NOT DEFINED EXPECT_EXIT)
    message(FATAL_ERROR "EXPECT_EXIT is required")
endif()

file(REMOVE "${OUTPUT}")

execute_process(
    COMMAND "${COMPILER}" "${INPUT}" -o "${OUTPUT}"
    RESULT_VARIABLE compile_result
    OUTPUT_VARIABLE compile_stdout
    ERROR_VARIABLE compile_stderr
)

if(NOT compile_result EQUAL 0)
    message(FATAL_ERROR
        "Coglet backend compilation failed with exit ${compile_result}\n"
        "stdout:\n${compile_stdout}\n"
        "stderr:\n${compile_stderr}"
    )
endif()

execute_process(
    COMMAND "${OUTPUT}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_stdout
    ERROR_VARIABLE run_stderr
)

if(NOT run_result MATCHES "^[0-9]+$")
    message(FATAL_ERROR
        "generated executable did not exit normally: ${run_result}\n"
        "stdout:\n${run_stdout}\n"
        "stderr:\n${run_stderr}"
    )
endif()

if(NOT run_result EQUAL EXPECT_EXIT)
    message(FATAL_ERROR
        "generated executable exited ${run_result}, expected ${EXPECT_EXIT}\n"
        "stdout:\n${run_stdout}\n"
        "stderr:\n${run_stderr}"
    )
endif()

if(DEFINED EXPECT_STDOUT_FILE)
    file(READ "${EXPECT_STDOUT_FILE}" expected_stdout)

    string(REPLACE "\r\n" "\n" run_stdout "${run_stdout}")
    string(REPLACE "\r\n" "\n" expected_stdout "${expected_stdout}")

    if(NOT run_stdout STREQUAL expected_stdout)
        message(FATAL_ERROR
            "generated executable stdout did not match ${EXPECT_STDOUT_FILE}\n"
            "expected:\n${expected_stdout}\n"
            "actual:\n${run_stdout}"
        )
    endif()
endif()
