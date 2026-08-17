if(NOT DEFINED COMPILER OR NOT DEFINED EXPECT_ROOT)
    message(FATAL_ERROR "COMPILER and EXPECT_ROOT are required")
endif()

execute_process(
    COMMAND "${COMPILER}" --print-stdlib-root
    OUTPUT_VARIABLE actual_stdout
    ERROR_VARIABLE actual_stderr
    RESULT_VARIABLE result
)

if(NOT result EQUAL 0)
    message(FATAL_ERROR "--print-stdlib-root exited ${result}, expected 0\n${actual_stderr}")
endif()
string(REPLACE "\r\n" "\n" actual_stdout "${actual_stdout}")
string(STRIP "${actual_stdout}" actual_stdout)
string(STRIP "${actual_stderr}" actual_stderr)
file(TO_CMAKE_PATH "${EXPECT_ROOT}" expected_root)
if(NOT actual_stdout STREQUAL expected_root)
    message(FATAL_ERROR
        "unexpected stdlib root\nexpected: '${expected_root}'\nactual:   '${actual_stdout}'")
endif()
if(NOT actual_stderr STREQUAL "")
    message(FATAL_ERROR "--print-stdlib-root wrote unexpected stderr:\n${actual_stderr}")
endif()
