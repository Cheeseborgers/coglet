if(NOT DEFINED EXE)
    message(FATAL_ERROR "EXE is required")
endif()

if(NOT DEFINED INPUT)
    message(FATAL_ERROR "INPUT is required")
endif()

if(NOT DEFINED EXPECTED)
    message(FATAL_ERROR "EXPECTED is required")
endif()

execute_process(
        COMMAND "${EXE}" "${INPUT}"
        OUTPUT_VARIABLE actual_stdout
        ERROR_VARIABLE actual_stderr
        RESULT_VARIABLE exit_code
)

if(NOT exit_code EQUAL 1)
    message(FATAL_ERROR
            "${EXE} exited with code ${exit_code} on ${INPUT}\n"
            "expected exit code: 1\n"
            "--- stdout ---\n${actual_stdout}\n"
            "--- stderr ---\n${actual_stderr}\n"
    )
endif()

if(NOT actual_stdout STREQUAL "")
    message(FATAL_ERROR
            "Expected no stdout for invalid parser input ${INPUT}\n"
            "--- stdout ---\n${actual_stdout}\n"
    )
endif()

# Parser diagnostics include the input path and source location. Snapshot only
# the stable diagnostic text/source excerpt so tests do not depend on checkout
# location.
string(REGEX REPLACE "[^\n]*:[0-9]+:[0-9]+: error:" "error:" normalized_stderr "${actual_stderr}")
string(REPLACE "\r\n" "\n" normalized_stderr "${normalized_stderr}")
string(STRIP "${normalized_stderr}" normalized_stderr)

file(READ "${EXPECTED}" expected_output)
string(REPLACE "\r\n" "\n" expected_output "${expected_output}")
string(STRIP "${expected_output}" expected_output)

if(NOT normalized_stderr STREQUAL expected_output)
    message(FATAL_ERROR
            "Mismatch for ${INPUT}\n"
            "--- expected ---\n${expected_output}\n"
            "--- actual ---\n${normalized_stderr}\n"
    )
endif()
