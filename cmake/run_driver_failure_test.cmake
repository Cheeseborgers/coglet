if(NOT DEFINED COMPILER)
    message(FATAL_ERROR "COMPILER is required")
endif()

if(NOT DEFINED INPUT)
    message(FATAL_ERROR "INPUT is required")
endif()

if(NOT DEFINED EXPECT_EXIT)
    message(FATAL_ERROR "EXPECT_EXIT is required")
endif()

if(NOT DEFINED EXPECT_SUBSTRING)
    message(FATAL_ERROR "EXPECT_SUBSTRING is required")
endif()

execute_process(
    COMMAND "${COMPILER}" "${INPUT}" -l coglet_backend_link_probe
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
