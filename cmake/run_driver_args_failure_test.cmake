if(NOT DEFINED COMPILER OR NOT DEFINED INPUT OR NOT DEFINED EXPECT_EXIT OR NOT DEFINED EXPECT_SUBSTRING)
    message(FATAL_ERROR "COMPILER, INPUT, EXPECT_EXIT, and EXPECT_SUBSTRING are required")
endif()

set(command "${COMPILER}" "${INPUT}")
if(DEFINED ARGS)
    list(APPEND command ${ARGS})
endif()

execute_process(
    COMMAND ${command}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
)
if(NOT result EQUAL EXPECT_EXIT)
    message(FATAL_ERROR
        "driver exited ${result}, expected ${EXPECT_EXIT}\n"
        "stdout:\n${stdout}\n"
        "stderr:\n${stderr}"
    )
endif()

string(FIND "${stderr}" "${EXPECT_SUBSTRING}" found)
if(found EQUAL -1)
    message(FATAL_ERROR
        "driver stderr did not contain '${EXPECT_SUBSTRING}'\n"
        "stderr:\n${stderr}"
    )
endif()
