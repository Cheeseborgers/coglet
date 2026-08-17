if(NOT DEFINED COMPILER OR NOT DEFINED EXPECT_VERSION)
    message(FATAL_ERROR "COMPILER and EXPECT_VERSION are required")
endif()

execute_process(
        COMMAND "${COMPILER}" --version
        OUTPUT_VARIABLE actual_stdout
        ERROR_VARIABLE actual_stderr
        RESULT_VARIABLE result
)

if(NOT result EQUAL 0)
    message(FATAL_ERROR
        "${COMPILER} --version exited ${result}, expected 0\n"
        "--- stdout ---\n${actual_stdout}\n"
        "--- stderr ---\n${actual_stderr}\n"
    )
endif()

string(REPLACE "\r\n" "\n" actual_stdout "${actual_stdout}")
string(STRIP "${actual_stdout}" actual_stdout)
string(STRIP "${actual_stderr}" actual_stderr)
set(expected_stdout "coglet ${EXPECT_VERSION}")

if(NOT actual_stdout STREQUAL expected_stdout)
    message(FATAL_ERROR
        "unexpected --version output\n"
        "expected: '${expected_stdout}'\n"
        "actual:   '${actual_stdout}'\n"
    )
endif()

if(NOT actual_stderr STREQUAL "")
    message(FATAL_ERROR
        "--version wrote unexpected stderr:\n${actual_stderr}\n"
    )
endif()
