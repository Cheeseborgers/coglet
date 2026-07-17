if(NOT DEFINED EXE)
    message(FATAL_ERROR "EXE is required")
endif()

if(NOT DEFINED INPUT)
    message(FATAL_ERROR "INPUT is required")
endif()

if(NOT DEFINED EXPECTED_STDOUT)
    message(FATAL_ERROR "EXPECTED_STDOUT is required")
endif()

if(NOT DEFINED EXPECTED_STDERR)
    message(FATAL_ERROR "EXPECTED_STDERR is required")
endif()

if(NOT DEFINED EXPECT_EXIT)
    message(FATAL_ERROR "EXPECT_EXIT is required")
endif()

execute_process(
        COMMAND "${EXE}" "${INPUT}"
        RESULT_VARIABLE actual_exit
        OUTPUT_VARIABLE actual_stdout
        ERROR_VARIABLE actual_stderr
)

file(READ "${EXPECTED_STDOUT}" expected_stdout)
file(READ "${EXPECTED_STDERR}" expected_stderr)

# Normalize Windows line endings so snapshots remain portable.
string(REPLACE "\r\n" "\n" actual_stdout "${actual_stdout}")
string(REPLACE "\r\n" "\n" actual_stderr "${actual_stderr}")
string(REPLACE "\r\n" "\n" expected_stdout "${expected_stdout}")
string(REPLACE "\r\n" "\n" expected_stderr "${expected_stderr}")

if(NOT actual_exit EQUAL EXPECT_EXIT)
    message(FATAL_ERROR
            "Unexpected exit code for ${INPUT}\n"
            "expected: ${EXPECT_EXIT}\n"
            "actual:   ${actual_exit}\n"
            "--- stdout ---\n"
            "${actual_stdout}\n"
            "--- stderr ---\n"
            "${actual_stderr}"
    )
endif()

if(NOT actual_stdout STREQUAL expected_stdout)
    string(LENGTH "${expected_stdout}" expected_stdout_length)
    string(LENGTH "${actual_stdout}" actual_stdout_length)

    message(FATAL_ERROR
            "stdout mismatch for ${INPUT}\n"
            "expected length=${expected_stdout_length}\n"
            "actual length=${actual_stdout_length}\n"
            "--- expected stdout ---\n"
            "${expected_stdout}"
            "--- actual stdout ---\n"
            "${actual_stdout}"
    )
endif()

if(NOT actual_stderr STREQUAL expected_stderr)
    string(LENGTH "${expected_stderr}" expected_stderr_length)
    string(LENGTH "${actual_stderr}" actual_stderr_length)

    message(FATAL_ERROR
            "stderr mismatch for ${INPUT}\n"
            "expected length=${expected_stderr_length}\n"
            "actual length=${actual_stderr_length}\n"
            "--- expected stderr ---\n"
            "${expected_stderr}"
            "--- actual stderr ---\n"
            "${actual_stderr}"
    )
endif()