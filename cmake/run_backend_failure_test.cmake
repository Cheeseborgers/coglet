if(NOT DEFINED COMPILER OR NOT DEFINED INPUT OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "COMPILER, INPUT, and OUTPUT are required")
endif()

if(NOT DEFINED EXPECT_EXIT OR NOT DEFINED EXPECT_SUBSTRING)
    message(FATAL_ERROR "EXPECT_EXIT and EXPECT_SUBSTRING are required")
endif()

file(REMOVE "${OUTPUT}")

execute_process(
    COMMAND "${COMPILER}" "${INPUT}" -o "${OUTPUT}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout_text
    ERROR_VARIABLE stderr_text
)

if(NOT result MATCHES "^[0-9]+$")
    message(FATAL_ERROR "compiler did not exit normally: ${result}")
endif()

if(NOT result EQUAL EXPECT_EXIT)
    message(FATAL_ERROR
        "compiler exited ${result}, expected ${EXPECT_EXIT}\n"
        "stdout:\n${stdout_text}\n"
        "stderr:\n${stderr_text}"
    )
endif()

string(FIND "${stderr_text}" "${EXPECT_SUBSTRING}" substring_index)
if(substring_index EQUAL -1)
    message(FATAL_ERROR
        "stderr did not contain expected text '${EXPECT_SUBSTRING}'\n"
        "stderr:\n${stderr_text}"
    )
endif()
