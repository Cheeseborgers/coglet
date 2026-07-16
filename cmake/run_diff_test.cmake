# Invoked by ctest as:
# cmake -DEXE=... -DINPUT=... -DEXPECTED=... -DEXPECT_EXIT=... -DOUTPUT_STREAM=... -P run_diff_test.cmake

if(NOT DEFINED EXPECT_EXIT)
    set(EXPECT_EXIT 0)
endif()

if(NOT DEFINED OUTPUT_STREAM)
    set(OUTPUT_STREAM stdout)
endif()

execute_process(
        COMMAND "${EXE}" "${INPUT}"
        OUTPUT_VARIABLE actual_stdout
        ERROR_VARIABLE actual_stderr
        RESULT_VARIABLE exit_code
)

if(NOT exit_code EQUAL EXPECT_EXIT)
    message(FATAL_ERROR
            "${EXE} exited with code ${exit_code} on ${INPUT}\n"
            "expected exit code: ${EXPECT_EXIT}\n"
            "--- stdout ---\n${actual_stdout}\n"
            "--- stderr ---\n${actual_stderr}\n"
    )
endif()

if(OUTPUT_STREAM STREQUAL "stdout")
    set(actual_output "${actual_stdout}")
elseif(OUTPUT_STREAM STREQUAL "stderr")
    set(actual_output "${actual_stderr}")
elseif(OUTPUT_STREAM STREQUAL "combined")
    set(actual_output "${actual_stdout}${actual_stderr}")
else()
    message(FATAL_ERROR
            "Invalid OUTPUT_STREAM: ${OUTPUT_STREAM}"
    )
endif()

file(READ "${EXPECTED}" expected_output)

string(REPLACE "\r\n" "\n" expected_output "${expected_output}")
string(REPLACE "\r\n" "\n" actual_output "${actual_output}")

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