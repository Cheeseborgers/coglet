# Invoked by ctest as: cmake -DEXE=... -DINPUT=... -DEXPECTED=... -P run_diff_test.cmake
# Runs EXE on INPUT, compares stdout against the contents of EXPECTED,
# and fails (non-zero exit) on any mismatch. Generic: used for both
# the lexer tests (dump_tokens) and parser tests (dump_ast).

execute_process(
    COMMAND ${EXE} ${INPUT}
    OUTPUT_VARIABLE actual_output
    RESULT_VARIABLE exit_code
)

if(NOT exit_code EQUAL 0)
    message(FATAL_ERROR "${EXE} exited with code ${exit_code} on ${INPUT}")
endif()

file(READ ${EXPECTED} expected_output)

string(REPLACE "\r\n" "\n" expected_output "${expected_output}")
string(REPLACE "\r\n" "\n" actual_output "${actual_output}")

# Strip trailing whitespace/newlines only, so the comparison isn't
# thrown off by editors adding/removing a final newline.
string(STRIP "${actual_output}" actual_output)
string(STRIP "${expected_output}" expected_output)

if(NOT actual_output STREQUAL expected_output)
    string(LENGTH "${expected_output}" expected_len)
    string(LENGTH "${actual_output}" actual_len)

    message(FATAL_ERROR
            "Mismatch for ${INPUT}\n"
            "expected length=${expected_len}\n"
            "actual length=${actual_len}\n"
            "--- expected ---\n${expected_output}\n"
            "--- actual ---\n${actual_output}\n"
    )
endif()
